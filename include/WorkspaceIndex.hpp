#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

// Phase 0 skeleton only (doc 01 P0.6, explicitly deferred scope there):
// enumerates every `.tr` file under the workspace root(s) reported at
// `initialize` so later work (doc 08 -- find references, rename, workspace
// symbols) has a ready file inventory to build a symbol/occurrence index
// over, instead of re-walking the filesystem from scratch when that lands.
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
    // and is deliberately the whole strategy for now; doc 08 can replace
    // this with incremental add/remove once it actually needs to.
    void rescan();

    std::vector<std::string> files() const;
    size_t fileCount() const;

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_folders;
    std::vector<std::string> m_files;
};
