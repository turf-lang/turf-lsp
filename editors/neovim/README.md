# Turf Language Server - Neovim Integration

This directory contains the Lua configuration snippet needed to attach `turf-lsp` to Neovim.

## Setup

1. Ensure the `turf-lsp` binary is built and available in your `$PATH`.
2. Add the contents of `turf-lsp.lua` to your `init.lua` (or require it).
3. Open a `.turf` file, and Neovim's built-in LSP client will start the server.
