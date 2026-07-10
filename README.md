# Turf Language Server (turf-lsp)

This directory contains the source code for the official Language Server Protocol (LSP) implementation for the Turf programming language. It is written in C++ and connects seamlessly with the `turflang` compiler core.

## Features

- **Standard LSP Compliance**: Implements the JSON-RPC 2.0 protocol based on the Microsoft Language Server Protocol specification.
- **Editor Agnostic**: Works out of the box with any editor supporting LSP, including:
  - Visual Studio Code
  - Neovim
  - Emacs
  - Sublime Text
- **Fast and Efficient**: Built in C++ to communicate directly with the `turflang` parsing and analysis libraries without FFI overhead.

## Quick Start

### Building

You need CMake and a C++17 compliant compiler. We use `nlohmann/json` for JSON parsing, which is automatically fetched by CMake.

```bash
mkdir build
cd build
cmake ..
make
```

### Editor Integration

See the `editors/` directory for specifics:
- [VSCode Integration](editors/vscode/README.md)
- [Neovim Integration](editors/neovim/README.md)

## Project Documentation

- **[ARCHITECTURE.md](ARCHITECTURE.md)**: Details the design of the language server, its main loop, and how it handles LSP requests.
- **[DEVELOPING.md](DEVELOPING.md)**: A guide for contributors on how to add new LSP features (e.g., hover, completions, go to definition).
