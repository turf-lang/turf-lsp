#include <iostream>
#include <string>
#include "LSPHandler.hpp"
#include "llvm/Support/FileSystem.h"

std::string CompilerExecutablePath;

int main(int argc, char** argv) {
    CompilerExecutablePath = llvm::sys::fs::getMainExecutable(argv[0], (void *)(intptr_t)main);

    // Disable synchronization with C-style I/O for performance and reliability
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    LSPHandler handler;

    while (std::cin.good()) {
        std::string line;
        std::getline(std::cin, line);
        
        // Remove trailing \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("Content-Length: ", 0) == 0) {
            std::string lengthStr = line.substr(16);
            int contentLength = std::stoi(lengthStr);

            // Read the empty line \r\n
            std::getline(std::cin, line); 

            // Read the JSON payload
            std::string payload(contentLength, ' ');
            std::cin.read(&payload[0], contentLength);

            // Dispatch to LSP handler
            handler.handleMessage(payload);
        }
    }

    return 0;
}
