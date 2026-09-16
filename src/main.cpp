#include <iostream>
#include <string>
#include "LSPFramer.hpp"
#include "LSPHandler.hpp"
#include "Logger.hpp"
#include "Version.h"
#include "llvm/Support/FileSystem.h"

namespace {

void printVersion() {
    std::cout << "turf-lsp " << TURF_LSP_VERSION
              << " (LSP protocol " << TURF_LSP_PROTOCOL_VERSION << ")\n";
}

void printHelp() {
    printVersion();
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
        "  TURF_HOME       Overrides the stdlib/installed-package resolution\n"
        "                  root normally derived from this binary's own path.\n";
}

} // namespace

// Required by turfcore itself (declared `extern` in AST.h, read by
// TurfInstallRoot() and SLM::fetchSuggestion()) -- NOT dead code, even
// though nothing in turf-lsp's own sources reads it directly. It's the
// compiler-binary-relative fallback TurfInstallRoot() uses to locate the
// stdlib (`lib/std/`) when neither an explicit `turf.home` setting nor
// TURF_HOME is set: same directory-relative layout the `turf` CLI itself
// assumes, just resolved from turf-lsp's own executable path instead of
// turf's.
std::string CompilerExecutablePath;

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--version") {
            printVersion();
            return 0;
        }
        if (arg == "-h" || arg == "--help") {
            printHelp();
            return 0;
        }
    }

    CompilerExecutablePath = llvm::sys::fs::getMainExecutable(argv[0], (void*)(intptr_t)main);

    Logger::init();

    // Disable synchronization with C-style I/O for performance and reliability
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    LSPHandler handler;

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
