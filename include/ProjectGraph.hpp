#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Knowing which project a file belongs to, so `import` resolves in the editor
// the way it does in `turf build`: the package's own modules anywhere under
// src/, its dependencies, the standard library -- not only the standard
// library and the files next to the open one.
//
// The compiler's query facade takes a package graph (the JSON `turf build`
// writes to build/package-graph.json); `turf graph` prints the same JSON for a
// tool to ask for, resolving the manifests and dependencies the way a build
// does. This asks it once per project, caches the answer, and asks again only
// when a manifest or lock file changed.
namespace turf_lsp {

struct ProjectGraph {
    // The graph file to hand the compiler; "" for single-file mode (the file
    // is in no project, is outside its project's src/ and tests/, or no `turf`
    // could be found).
    std::string graphFile;
    // Why a project's graph could not be made, in `turf`'s words ("" when it
    // could, and when there is nothing to report). Shown on the file as an
    // error: a manifest that does not parse is the user's to fix.
    std::string problem;
    // The project directory (where turf.toml is); "" when there is none.
    std::string root;
};

// The directory holding turf.toml, searching upward from the file's directory;
// "" when there is none.
std::string findProjectRoot(const std::string& file);

// Is `path` inside `dir` (a path-component prefix, after resolving `..` and
// symlinks where it can)? "src2" is not inside "src".
bool isInside(const std::string& path, const std::string& dir);

// Where `turf` is, for asking it for a graph:
//   1. $TURF_CLI, if set (an explicit choice, used as given);
//   2. next to the server (`<root>/bin/turf` alongside `<root>/bin/turf-lsp`);
//   3. `<stdlibRoot>/bin/turf` -- an installed toolchain;
//   4. `turf` on PATH.
// "" when none exists: the server then analyses files on their own.
std::string findTurfExecutable(const std::string& serverExe, const std::string& stdlibRoot);

class ProjectGraphProvider {
public:
    ProjectGraphProvider() = default;
    explicit ProjectGraphProvider(std::string turfExe) : m_turf(std::move(turfExe)) {}

    void setTurfExecutable(std::string turfExe) { m_turf = std::move(turfExe); }
    const std::string& turfExecutable() const { return m_turf; }

    // The graph for `file`, running `turf graph` if the project's manifests
    // changed since it was last asked. Worker thread only (it spawns a process
    // and is not synchronised).
    ProjectGraph forFile(const std::string& file);

private:
    struct Entry {
        std::string signature;                  // the manifests' stamps (time, size, hash) when it was made
        ProjectGraph result;                    // for files inside this project
        std::vector<std::string> packageDirs;   // every package's directory in the graph
    };

    Entry build(const std::string& root);

    std::string m_turf;
    std::map<std::string, Entry> m_byRoot;
};

} // namespace turf_lsp
