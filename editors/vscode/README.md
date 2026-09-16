# Turf Language Server - VSCode Extension

This is the VSCode extension wrapper for `turf-lsp`.

## Setup

1. Run `npm install` in this directory.
2. Compile the extension with `npm run compile`.
3. Open this folder in VSCode and press `F5` to launch the Extension Development Host.

## Binary discovery

The extension looks for the `turf-lsp` binary in this order:
1. The `turf.server.path` setting, if set.
2. A `build/turf-lsp` next to the open workspace (several common repo layouts:
   the workspace *is* `turf-lsp/`, or is its parent containing both
   `turf-lang/` and `turf-lsp/`, or is `turf-lang/` with `turf-lsp/` checked
   out as a sibling).
3. A dev-mode fallback relative to this extension's own install location
   (useful when F5-launching against an unrelated workspace).
4. `turf-lsp` on `PATH`, with a warning shown if none of the above matched.

## Configuration

```json
{
    "turf.server.path": "/absolute/path/to/turf-lsp/build/turf-lsp",
    "turf.home": "",
    "turf.trace.server": "off",
    "turf.diagnostics.enable": true,
    "turf.format.onSave": false,
    "turf.completion.snippets": true
}
```

- `turf.home` is passed to the server process as the `TURF_HOME` environment
  variable, used to resolve stdlib/installed-package imports.
- `turf.format.onSave` runs the server's `textDocument/formatting` on every
  save of a `.tr` file.

Use the **Turf: Restart Language Server** command (also bound to the status
bar item) after changing settings that require a restart. Changing
`turf.server.path` or `turf.home` restarts the server automatically.

Server stderr and LSP trace output (when `turf.trace.server` is not `off`)
appear in the **Turf Language Server** output channel.
