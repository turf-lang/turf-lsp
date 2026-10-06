#include "ProjectGraph.hpp"

#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>

#include <nlohmann/json.hpp>

#include "llvm/Support/Program.h"

#include "Logger.hpp"

namespace turf_lsp {

namespace fs = std::filesystem;

namespace {

std::string canonical(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(path, ec);
    return (ec ? fs::path(path).lexically_normal() : p).string();
}

bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The first few lines of `turf`'s complaint, which can be long (a dependency
// that cannot be fetched lists what it tried).
std::string firstLines(const std::string& text, size_t maxLines) {
    std::istringstream in(text);
    std::string line, out;
    size_t n = 0;
    while (n < maxLines && std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        out += (out.empty() ? "" : "\n") + line;
        ++n;
    }
    return out;
}

std::string mtimeOf(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    return ec ? std::string("-") : std::to_string(t.time_since_epoch().count());
}

// What a graph depends on: the root's manifest and lock, and every package's
// manifest (a path dependency's `turf.toml` can change the graph too).
std::string signatureOf(const fs::path& root, const std::vector<std::string>& packageDirs) {
    std::string sig = mtimeOf(root / "turf.toml") + "|" + mtimeOf(root / "turf.lock");
    for (const auto& dir : packageDirs) sig += "|" + dir + ":" + mtimeOf(fs::path(dir) / "turf.toml");
    return sig;
}

fs::path cacheDir() {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "turf-lsp-graphs";
    fs::create_directories(dir, ec);
    return dir;
}

const char* kTurfName =
#ifdef _WIN32
    "turf.exe";
#else
    "turf";
#endif

} // namespace

std::string findProjectRoot(const std::string& file) {
    std::error_code ec;
    fs::path dir = fs::path(canonical(file)).parent_path();
    for (int depth = 0; depth < 64 && !dir.empty(); ++depth) {
        if (isFile(dir / "turf.toml")) return dir.string();
        fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = std::move(parent);
    }
    return "";
}

bool isInside(const std::string& path, const std::string& dir) {
    const std::string p = canonical(path);
    std::string d = canonical(dir);
    if (d.empty()) return false;
    const char sep = static_cast<char>(fs::path::preferred_separator);
    if (d.back() != sep) d += sep;
    return p.size() > d.size() && p.compare(0, d.size(), d) == 0;
}

std::string findTurfExecutable(const std::string& serverExe, const std::string& stdlibRoot) {
    if (const char* env = std::getenv("TURF_CLI"); env && *env) return env;

    std::vector<fs::path> candidates;
    if (!serverExe.empty()) candidates.push_back(fs::path(serverExe).parent_path() / kTurfName);
    if (!stdlibRoot.empty()) candidates.push_back(fs::path(stdlibRoot) / "bin" / kTurfName);
    for (const auto& c : candidates)
        if (isFile(c)) return c.string();

    if (auto found = llvm::sys::findProgramByName("turf")) return *found;
    return "";
}

ProjectGraph ProjectGraphProvider::forFile(const std::string& file) {
    ProjectGraph none;
    const std::string root = findProjectRoot(file);
    if (root.empty()) return none;
    none.root = root;
    if (m_turf.empty()) return none;

    // The graph describes the root package: only its src/ and tests/ are
    // analysable with it (the compiler refuses any other entry file). A file
    // elsewhere in the project directory -- examples/, a scratch file -- is
    // analysed on its own.
    if (!isInside(file, (fs::path(root) / "src").string()) &&
        !isInside(file, (fs::path(root) / "tests").string()))
        return none;

    auto it = m_byRoot.find(root);
    if (it != m_byRoot.end() && it->second.signature == signatureOf(root, it->second.packageDirs))
        return it->second.result;

    Entry entry = build(root);
    return (m_byRoot[root] = std::move(entry)).result;
}

ProjectGraphProvider::Entry ProjectGraphProvider::build(const std::string& root) {
    Entry entry;
    entry.result.root = root;

    const fs::path dir = cacheDir();
    const std::string key = [&] {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%016zx", std::hash<std::string>{}(root));
        return std::string(buf);
    }();
    const fs::path out = dir / (key + ".json");
    const fs::path tmp = dir / (key + ".json.tmp");
    const fs::path err = dir / (key + ".err");

    // --offline: an editor must never reach for the network on its own; a git
    // dependency that is not in the fetch cache is reported, and `turf fetch`
    // (or a build) puts it there.
    // (The redirect paths are kept in named strings: a StringRef to a
    // temporary would dangle before the process starts.)
    const std::string tmpPath = tmp.string(), errPath = err.string();
    std::vector<llvm::StringRef> args = {m_turf, "graph", "--project", root, "--offline"};
    std::optional<llvm::StringRef> redirects[3] = {std::nullopt, llvm::StringRef(tmpPath),
                                                    llvm::StringRef(errPath)};
    std::string execError;
    int status = llvm::sys::ExecuteAndWait(m_turf, args, std::nullopt, redirects, /*SecondsToWait=*/30,
                                           /*MemoryLimit=*/0, &execError);

    std::error_code ec;
    if (status == 0) {
        fs::rename(tmp, out, ec);
        if (ec) {
            fs::copy_file(tmp, out, fs::copy_options::overwrite_existing, ec);
            fs::remove(tmp, ec);
        }
        entry.result.graphFile = out.string();
        try {
            auto graph = nlohmann::json::parse(readFile(out));
            for (const auto& [name, pkg] : graph.at("packages").items())
                entry.packageDirs.push_back(fs::path(pkg.at("src").get<std::string>()).parent_path().string());
        } catch (const std::exception& e) {
            // The compiler will say what is wrong with it; nothing to track.
            Logger::warn(std::string("turf graph output was not a package graph: ") + e.what());
        }
        Logger::info("project graph for " + root + " (" + std::to_string(entry.packageDirs.size()) + " package(s))");
    } else {
        std::string why = firstLines(readFile(err), 6);
        if (why.empty()) why = execError.empty() ? "exit status " + std::to_string(status) : execError;
        entry.result.problem = "turf graph failed for the project at " + root + ":\n" + why;
        Logger::warn(entry.result.problem);
        fs::remove(tmp, ec);
    }
    fs::remove(err, ec);
    entry.signature = signatureOf(root, entry.packageDirs);
    return entry;
}

} // namespace turf_lsp
