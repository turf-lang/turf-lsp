# Turf Language Server - Sublime Text Integration

Config snippet only, via the `LSP` package. Install `LSP`
(Package Control), then add a `Packages/User/LSP.sublime-settings` (Sublime
menu: `Preferences > Package Settings > LSP > Settings`):

```json
{
  "clients": {
    "turf-lsp": {
      "enabled": true,
      "command": ["turf-lsp"],
      "selector": "source.turf"
    }
  }
}
```

The server finds the standard library next to itself when it sits in a `bin/`
directory with `../lib/std` (see the [main README](../../README.md)), so no
`TURF_HOME` is needed. To use a different installation's library, add
`"env": {"TURF_HOME": "/path/to/turf/home"}` alongside `"command"`.

## Syntax highlighting gap

This client-registration snippet is spec-compliant and sufficient for
diagnostics/hover/completion/go-to-def, but the `source.turf` scope it
selects on doesn't exist in Sublime yet — there's no shipped Sublime
syntax definition for Turf. `syntaxes/turf.tmLanguage.json` (the grammar
used by the VS Code extension) is **not** a drop-in Sublime
`.sublime-syntax` file; porting or converting it is unstarted, out of
scope for this pass. Until that exists, `.tr` files get LSP features but
plain-text highlighting.
