# Developing `turf-lsp`

Welcome! This guide will help you start contributing to the Turf Language Server.

## Prerequisites

- **CMake** (v3.15+)
- **C++ Compiler** (C++20 support required: GCC 10+, Clang 12+, or MSVC)
- A basic understanding of the [Language Server Protocol Specification](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/).

## Build Instructions

1. Create a build directory:
   ```bash
   mkdir build && cd build
   ```
2. Configure the project using CMake:
   ```bash
   cmake ..
   ```
   *Note: This will automatically download and configure `nlohmann/json`.*
3. Build the executable:
   ```bash
   make
   ```
4. The binary `turf-lsp` will be generated in the `build/` directory.

## How to Add a New LSP Feature

Let's say you want to add support for the **Hover** feature (`textDocument/hover`).

1. **Update Server Capabilities**: 
   When the client sends the `initialize` request, the server responds with its capabilities. Open `LSPHandler.cpp` and modify the `initialize` response to include:
   ```json
   "hoverProvider": true
   ```
2. **Add a Method Handler**:
   In `LSPHandler.cpp`, add an `else if` branch inside the `handleMessage` method:
   ```cpp
   if (method == "textDocument/hover") {
       handleHover(jsonMessage);
   }
   ```
3. **Implement the Logic**:
   Create the `handleHover` method. It should:
   - Extract the URI and position (line, character) from the parameters.
   - Query the Turf AST (via `turflang` libraries) to find what is at that position.
   - Construct a `Hover` JSON-RPC response and send it back to the client using `sendMessage()`.

## Debugging

Since `turf-lsp` communicates via `stdin` and `stdout`, you cannot easily attach a standard debugger or print to `stdout` (as it will corrupt the JSON-RPC stream).

**Best Practices for Debugging**:
- Use `std::cerr` for logging. The editors (VSCode, Neovim) usually capture `stderr` from the language server and display it in an output channel.
- Create a file logger that writes to `/tmp/turf-lsp.log`.
