#include "WorkspaceIndex.hpp"
#include "Logger.hpp"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

namespace {

bool shouldSkipDir(llvm::StringRef name) {
    // `.git`, `.cache`, any other dotdir, plus the two build-output names
    // this repo (and turf-lang) actually use. Hidden-dir skipping also
    // naturally keeps `.claude/`, editor swap dirs, etc. out of the index.
    if (name.starts_with(".")) return true;
    return name == "build" || name == "turflang_build";
}

void walkFolder(const std::string& root, std::vector<std::string>& out) {
    std::error_code ec;
    llvm::sys::fs::recursive_directory_iterator it(root, ec), end;
    if (ec) {
        Logger::warn("WorkspaceIndex: cannot open workspace folder '" + root + "': " + ec.message());
        return;
    }
    for (; it != end && !ec; it.increment(ec)) {
        llvm::StringRef path = it->path();
        llvm::StringRef base = llvm::sys::path::filename(path);

        auto statOrErr = it->status();
        if (!statOrErr) continue;
        if (statOrErr->type() == llvm::sys::fs::file_type::directory_file) {
            if (shouldSkipDir(base)) it.no_push();
            continue;
        }
        if (statOrErr->type() != llvm::sys::fs::file_type::regular_file) continue;
        if (llvm::sys::path::extension(path) == ".tr") {
            out.emplace_back(path.str());
        }
    }
    if (ec) {
        Logger::warn("WorkspaceIndex: error while walking '" + root + "': " + ec.message());
    }
}

} // namespace

void WorkspaceIndex::setWorkspaceFolders(std::vector<std::string> folderPaths) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_folders = std::move(folderPaths);
}

void WorkspaceIndex::rescan() {
    std::vector<std::string> folders;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        folders = m_folders;
    }

    std::vector<std::string> files;
    for (const auto& folder : folders) {
        walkFolder(folder, files);
    }

    Logger::info("WorkspaceIndex: indexed " + std::to_string(files.size()) + " .tr file(s) across " +
                 std::to_string(folders.size()) + " workspace folder(s)");

    std::lock_guard<std::mutex> lock(m_mutex);
    m_files = std::move(files);
}

std::vector<std::string> WorkspaceIndex::files() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_files;
}

size_t WorkspaceIndex::fileCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_files.size();
}
