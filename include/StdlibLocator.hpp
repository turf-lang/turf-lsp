#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Finding the Turf standard library (lib/std) from wherever the server was
// installed, so that "install the extension and it works" does not depend on
// the user setting anything.
//
// The compiler resolves `import io;` (and the nine prelude modules every file
// imports implicitly) against an installation root: $TURF_HOME if set, else
// the directory above the running binary. Both are trusted blindly, so a
// stray TURF_HOME pointing at the wrong place used to make every import fail,
// and no lib/std at all used to end the process. This validates the candidates
// instead of trusting them, picks the first that really contains the library,
// and says plainly when there is none.
namespace turf_lsp {

// Everything the search reads from the machine, as plain values, so a test
// can drive it with any layout (Linux, macOS, Windows paths) without touching
// the process's environment.
struct StdlibEnv {
    std::string turfHome;     // $TURF_HOME ("" when unset)
    std::string exePath;      // this binary (absolute)
    std::string home;         // the user's home directory
    std::string localAppData; // %LOCALAPPDATA% (Windows; "" elsewhere)
};

struct StdlibLocation {
    std::string root; // the installation root; "" when none was found
    std::string how;  // what chose it, for the log and `--version`
    // One line per candidate that was set but unusable (e.g. a TURF_HOME with
    // no lib/std in it): never silently ignored.
    std::vector<std::string> warnings;
    // Every root that was looked at, in order, for the "not found" message.
    std::vector<std::string> searched;

    bool found() const { return !root.empty(); }
};

// True when `root` holds a standard library: lib/std/core exists, which is
// where the prelude modules live and so the part nothing works without.
bool hasStdlib(const std::filesystem::path& root);

// The candidates, in order:
//   1. $TURF_HOME, if it is set and valid. An explicit choice wins; one that
//      is set but wrong is reported and skipped, never trusted.
//   2. The directory above the binary (`<root>/bin/turf-lsp`): the layout of
//      an installed toolchain and of the extension's bundled server.
//   3. The binary's own directory (`<root>/turf-lsp`).
//   4. The per-user install of the toolchain: ~/.turf, %LOCALAPPDATA%\turf.
StdlibLocation locateStdlib(const StdlibEnv& env);

// The real environment of this process.
StdlibEnv currentStdlibEnv(const std::string& exePath);

// Makes the compiler use `root`: it reads $TURF_HOME and trusts it.
void exportStdlibRoot(const std::string& root);

} // namespace turf_lsp
