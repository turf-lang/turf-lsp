#include <iostream>
#include <string>
#include "LSPFramer.hpp"
#include "LSPHandler.hpp"
#include "Logger.hpp"
#include "Version.h"
#include "AST.h" // CompilerExecutablePath
#include "llvm/Support/FileSystem.h"

namespace {

// The first line is the stable part (`turf-lsp <version> (LSP protocol ..)`);
// the lines after it say what this server is built from and where it found
// the standard library, which is the first thing to ask when imports fail.
void printVersion(const turf_lsp::StdlibLocation& stdlib) {
    std::cout << "turf-lsp " << TURF_LSP_VERSION
              << " (LSP protocol " << TURF_LSP_PROTOCOL_VERSION << ")\n"
              << "compiler " << TURF_LSP_COMPILER_REV << "\n";
    if (stdlib.found())
        std::cout << "stdlib " << stdlib.root << " (" << stdlib.how << ")\n";
    else
        std::cout << "stdlib not found\n";
    for (const auto& w : stdlib.warnings) std::cout << "warning: " << w << "\n";
}

void printHelp(const turf_lsp::StdlibLocation& stdlib) {
    printVersion(stdlib);
    std::cout <<
        "\n"
        "Language Server for the Turf programming language.\n"
        "\n"
        "Communicates over stdio using the Language Server Protocol -- it is\n"
        "meant to be launched by an editor/client, not run interactively.\n"
        "See editors/vscode/ and editors/neovim/ for client configuration.\n"
        "\n"
        "Usage: turf-lsp [options]\n"
        "\n"
        "Options:\n"
        "  -v, --version   Print the server version and exit.\n"
        "  -h, --help      Print this help and exit.\n"
        "\n"
        "Environment:\n"
        "  TURF_HOME       A Turf installation to take the standard library\n"
        "                  from. Used only if it contains lib/std/core;\n"
        "                  otherwise the library is looked for next to this\n"
        "                  binary (../lib/std), then in ~/.turf.\n";
}

} // namespace

int main(int argc, char** argv) {
    // CompilerExecutablePath is defined by turfcore (declared `extern` in
    // AST.h). TurfInstallRoot() falls back to it, when neither TURF_HOME nor
    // an explicit setting is given, to find the stdlib (`lib/std/`) relative
    // to this binary: set it before any compiler code runs.
    CompilerExecutablePath = llvm::sys::fs::getMainExecutable(argv[0], (void*)(intptr_t)main);

    // Find the standard library before any compiler code runs, and point the
    // compiler at it. See StdlibLocator.hpp for why this is validated rather
    // than left to $TURF_HOME and the binary's location. (Before the flags
    // below, so --version can say what it found.)
    turf_lsp::StdlibLocation stdlib = turf_lsp::locateStdlib(turf_lsp::currentStdlibEnv(CompilerExecutablePath));

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--version") {
            printVersion(stdlib);
            return 0;
        }
        if (arg == "-h" || arg == "--help") {
            printHelp(stdlib);
            return 0;
        }
    }

    Logger::init();
    for (const auto& w : stdlib.warnings) Logger::warn(w);
    if (stdlib.found()) {
        turf_lsp::exportStdlibRoot(stdlib.root);
        Logger::info("turf-lsp: standard library at " + stdlib.root + " (" + stdlib.how + ")");
    } else {
        Logger::error("turf-lsp: the standard library (lib/std) was not found");
    }

    // Disable synchronization with C-style I/O for performance and reliability
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    LSPHandler handler;
    handler.setStdlib(stdlib);

    // This loop IS the I/O thread (doc 01 P0.1): it only frames messages
    // and hands them to the handler, which itself only does cheap,
    // non-compiler bookkeeping before returning -- any compiler-touching
    // work handleMessage triggers runs asynchronously on LSPHandler's own
    // worker thread. Never call anything compiler-related directly from
    // this loop.
    while (auto message = LSPFramer::readMessage(std::cin)) {
        handler.handleMessage(*message);
    }

    Logger::info("turf-lsp: input stream closed, exiting.");
    return 0;
}
