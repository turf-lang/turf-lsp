# Turf in Sublime Text

Configuration for the [`LSP`](https://packagecontrol.io/packages/LSP) package;
nothing else to install.

Get the server first: take `turf-lsp-<platform>-<version>` from a
[release](https://github.com/turf-lang/turf-lsp/releases) and use the full path to
`bin/turf-lsp` in `command` below, or put `bin/` on your `PATH`. Keep the archive's
layout: the server finds the standard library at `../lib/std` from its own
directory. See the [main README](../../README.md#other-editors).

## 1. A syntax for `.tr` files

The `LSP` package attaches a server to buffers by their syntax scope, and Sublime
has no Turf syntax. Without one the server never starts. Save this as
`Packages/User/Turf.sublime-syntax` (Preferences > Browse Packages...). It defines
the scope and the file extension and highlights nothing:

```yaml
%YAML 1.2
---
name: Turf
file_extensions: [tr]
scope: source.turf
contexts:
  main: []
```

The TextMate grammar in `editors/vscode/syntaxes/turf.tmLanguage.json` is not a
`.sublime-syntax` file and cannot be dropped in. If you write a fuller syntax, keep
`scope: source.turf`.

## 2. Register the server

Open Preferences > Package Settings > LSP > Settings and add:

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

To take the standard library from a different installation, add
`"env": {"TURF_HOME": "/path/to/turf/home"}` next to `command`.

Diagnostics, hover, completion, go to definition, references, rename and formatting
then work through the package's own commands. The server's semantic tokens are not
used by Sublime, so highlighting is whatever the syntax gives.
