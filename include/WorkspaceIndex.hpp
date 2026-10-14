#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

// The inventory of `.tr` files under the workspace root(s) reported at
// `initialize`, which workspace-wide references, rename and workspace symbol
// search scan, instead of each re-walking the filesystem.
// Deliberately does NOT parse or analyze any file here -- just the
// inventory, and only ever off the I/O thread's own bookkeeping (never
// touches the compiler, so it doesn't need the analysis worker).
class WorkspaceIndex {
public:
    // `folderPaths` are plain filesystem paths (already stripped of the
    // `file://` scheme by the caller), one per `workspaceFolders` entry
    // from `initialize`.
    void setWorkspaceFolders(std::vector<std::string> folderPaths);

    // Walks all registered folders and (re)builds the file list. Safe to
    // call again later, e.g. in response to a future
    // `workspace/didChangeWatchedFiles` notification for a create/delete --
    // a full rescan is cheap (stats paths only, never reads file content)
    // and is deliberately the whole strategy; incremental add/remove can replace
    // it if a rescan ever becomes too slow.
    void rescan();

    std::vector<std::string> files() const;
    size_t fileCount() const;

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_folders;
    std::vector<std::string> m_files;
};
