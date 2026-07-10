# Turf Language Server - VSCode Extension

This is the VSCode extension wrapper for `turf-lsp`. 

## Setup

1. Run `npm install` in this directory.
2. Compile the extension with `npm run compile`.
3. Open this folder in VSCode and press `F5` to launch the Extension Development Host.

## Configuration

You can configure the path to the LSP binary in your settings:
```json
{
    "turf.server.path": "/absolute/path/to/turf-lsp/build/turf-lsp"
}
```
