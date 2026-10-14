# turf-lsp

The language server for the [Turf programming language](https://github.com/turf-lang/turf-lang),
with a VS Code extension and setup instructions for Neovim, Emacs and Sublime
Text. It speaks the Language Server Protocol 3.17 over stdio.

The server links the compiler's front end (TurfLang 2.0.0) in-process, through the
compiler's query facade, so diagnostics, hover, completion and the rest come from
the compiler itself and not from a second implementation of the language.

## Install

### VS Code

Install **Turf** (`turflang.turf`) from the Visual Studio Marketplace, or from
Open VSX if you use VSCodium, Cursor, Windsurf or another fork, and open a `.tr`
file. The extension carries the language server and the standard library, so there
is nothing else to install or configure. It is published for Linux x64, Windows x64
and macOS on Apple silicon. See [editors/vscode/README.md](editors/vscode/README.md)
for its commands and settings.

Editing needs nothing more. Running a program needs the Turf toolchain (the `turf`
command), which comes from the [compiler repository](https://github.com/turf-lang/turf-lang).

### Other editors

Any editor with an LSP client can use the server. Download
`turf-lsp-<platform>-<version>` from a [GitHub release](https://github.com/turf-lang/turf-lsp/releases),
unpack it and keep its layout:

```
turf-lsp-<platform>-<version>/
  bin/turf-lsp      the server
  bin/turf          the project helper, needed for turf.toml projects
  lib/std/...       the standard library
```

The server looks for the standard library next to itself (`../lib/std`), then in
`$TURF_HOME`, then in `~/.turf`. Point your editor at `bin/turf-lsp`, then follow
your editor's page:

- [Neovim](editors/neovim/README.md), also installable through Mason once
  the registry entry is merged
- [Emacs](editors/emacs/README.md) (`eglot` and `lsp-mode`)
- [Sublime Text](editors/sublime/README.md) (the `LSP` package)

`turf-lsp --version` prints what the server found:

```
turf-lsp 1.0.2 (LSP protocol 3.17)
compiler d54a43c5b6f0
stdlib /opt/turf (next to the server (../lib/std))
```

If the standard library is missing the server stays up and says so (a message to
the editor, and `E027` on every `import`) instead of failing silently.

## Features

- **Diagnostics** as you type: the compiler's errors with their codes and hints,
  and the lint and flow-analysis warnings `turf` prints, each underlined across the
  whole token.
- **Completion** for what is in scope at the cursor, `module::` members (only the
  `pub` ones), `instance.` members, module names after `import`, and keywords and
  templates for where you are. Typing the name of a module you have not imported
  and accepting it adds the `import`.
- **Signature help** for functions, module functions, static and instance methods,
  and every overload.
- **Inlay hints** with the parameter name before each argument.
- **Hover**, **go to definition, declaration and type definition**, **references**
  across the workspace, **document highlight** and **rename** (which refuses the
  standard library).
- **Outline**, **workspace symbol search**, **folding**, **document links** on
  `import`, and **semantic highlighting**.
- **Quick fixes** to add a missing `import`, organize imports, and prefix an unused
  variable with `_`.
- **Formatting** of a document or a selection, changing only the lines that differ.
- **Projects**: in a `turf.toml` project, `import` resolves as in `turf build`
  (your modules under `src/`, dependencies, `tests/`). Unsaved edits to one file
  are seen by the files that import it, and a manifest that does not parse is
  reported on the file.

Not provided: call and type hierarchy, a debug adapter (gdb works on `-g` builds),
and inferred-type inlay hints, since Turf declarations spell their types.

## Troubleshooting

In VS Code, click **Turf** in the status bar for what is running, and open
**Output -> Turf Language Server** for the log. Set `turf.trace.server` to
`verbose` to see the protocol traffic. Elsewhere, the server logs to stderr, and to
a file if `TURF_LSP_LOG` names one.

| Symptom | Cause and fix |
|---|---|
| "the standard library was not found" | The server found no `lib/std` next to itself, in `TURF_HOME` or in `~/.turf`. Reinstall the extension, or keep the `bin/` and `lib/` layout above. |
| Imports of your own modules are red | They need a `turf.toml` at the project root (`turf new` makes one), and the `turf` helper next to the server or on `PATH`. A manifest that does not parse is reported on the file. |
| `turf-lsp` exits at once on Linux | A development build links the shared LLVM. Build with `-DTURF_STATIC_LLVM=ON`, or install LLVM 22. Released binaries are static. |

## Contributing

Build instructions, the test layers, CI and the release process are in
[DEVELOPING.md](DEVELOPING.md). How the server is put together, and why, is in
[ARCHITECTURE.md](ARCHITECTURE.md). [SYNTAX_REFERENCE.md](SYNTAX_REFERENCE.md) is a
short summary of the language as the highlighting and completion know it.

## Releases

Pushing a tag `vX.Y.Z` builds every platform, attaches the `.vsix` files and the
server archives to a GitHub release, and publishes the extension to the Visual
Studio Marketplace and Open VSX. A nightly workflow builds the server against the
compiler's `master` and opens an issue when the two drift apart.

The project is licensed under Apache-2.0.
