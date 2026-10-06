#include "StdlibLocator.hpp"

#include <cstdlib>

namespace turf_lsp {

namespace fs = std::filesystem;

bool hasStdlib(const fs::path& root) {
    std::error_code ec;
    return fs::is_directory(root / "lib" / "std" / "core", ec);
}

StdlibLocation locateStdlib(const StdlibEnv& env) {
    StdlibLocation out;

    // `how` is what the log and `--version` show; the root is cleaned of
    // `..` so it reads as a path a person would recognise.
    auto consider = [&](const fs::path& candidate, const std::string& how) {
        if (out.found()) return;
        // `<root>/bin/..` normalises to `<root>/`; the root reads without it.
        fs::path normal = candidate.lexically_normal();
        if (!normal.has_filename() && normal.has_parent_path()) normal = normal.parent_path();
        const std::string shown = normal.string();
        out.searched.push_back(shown);
        if (hasStdlib(candidate)) {
            out.root = shown;
            out.how = how;
        }
    };

    if (!env.turfHome.empty()) {
        const fs::path home(env.turfHome);
        if (hasStdlib(home)) {
            consider(home, "TURF_HOME");
        } else {
            // An explicit choice that is wrong: reported and skipped.
            out.warnings.push_back("TURF_HOME is set to '" + env.turfHome +
                                   "' but there is no lib/std/core in it; ignoring it");
            out.searched.push_back(env.turfHome);
        }
    }

    if (!env.exePath.empty()) {
        const fs::path exeDir = fs::path(env.exePath).parent_path();
        consider(exeDir / "..", "next to the server (../lib/std)");
        consider(exeDir, "beside the server (lib/std)");
    }
    if (!env.home.empty()) consider(fs::path(env.home) / ".turf", "the user's Turf install (~/.turf)");
    if (!env.localAppData.empty()) consider(fs::path(env.localAppData) / "turf", "the user's Turf install (%LOCALAPPDATA%\\turf)");

    return out;
}

StdlibEnv currentStdlibEnv(const std::string& exePath) {
    auto get = [](const char* name) {
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string();
    };
    StdlibEnv env;
    env.turfHome = get("TURF_HOME");
    env.exePath = exePath;
    env.home = get("HOME");
    if (env.home.empty()) env.home = get("USERPROFILE");
    env.localAppData = get("LOCALAPPDATA");
    return env;
}

void exportStdlibRoot(const std::string& root) {
#ifdef _WIN32
    _putenv_s("TURF_HOME", root.c_str());
#else
    setenv("TURF_HOME", root.c_str(), 1);
#endif
}

} // namespace turf_lsp
