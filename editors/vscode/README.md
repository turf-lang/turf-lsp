# Turf for Visual Studio Code

Language support for [Turf](https://github.com/turf-lang/turf-lang): errors and
warnings as you type, completion, hover, go to definition, rename, formatting and
more. Open a `.tr` file and start writing. The language server and the standard
library come with the extension, so there is nothing to install and nothing to
configure.

The answers come from the Turf compiler itself (TurfLang 2.0.0), running inside the
language server, so the editor agrees with `turf build`.

## Features

- **Diagnostics** with the compiler's own explanations and error codes, plus
  warnings such as unused variables and unreachable code.
- **Completion** for variables and functions in scope, struct fields and methods,
  `module::` members (only what a module makes `pub`), module names after `import`,
  and keywords and templates for the statement you are on. Type the name of a module
  you have not imported and accepting it adds the `import`.
- **Signature help** while typing a call's arguments, and **inlay hints** that show
  the parameter name before each argument.
- **Hover**, **go to definition** (into the standard library and your own modules),
  **find references**, **rename**, **document highlights**, **outline**,
  **workspace symbol search** (`Ctrl+T`), and **document links** (`Ctrl+click` an
  `import`).
- **Quick fixes** (`Ctrl+.`): add a missing `import`, organize imports, and prefix an
  unused variable with `_`.
- **Formatting** of the document or a selection, changing only the lines that differ.
- **Folding**, **semantic highlighting**, and a status bar item showing what is
  running.
- **Projects**: in a folder with a `turf.toml`, imports resolve as in `turf build`,
  covering your own modules under `src/`, your dependencies (a git dependency once
  `turf fetch` or a build has fetched it) and `tests/`. Unsaved
  edits to one file are seen by the files that import it.

## Requirements

None for editing. The extension is published for Linux x64, Windows x64 and macOS on
Apple silicon, with the server and standard library bundled for each.

Running a program needs the Turf toolchain, the `turf` command, which comes from the
[Turf compiler repository](https://github.com/turf-lang/turf-lang). Without it, Run,
Build, Check and Test tell you so and show how to install it, and everything else
keeps working. If `turf` is installed somewhere unusual, set `turf.toolchain.path`.

## Running programs

- **Turf: Run**, **Turf: Build**, **Turf: Check** and **Turf: Run Tests** are
  available from the command palette, the Run button in the editor title, and links
  above `main` and in test files. They run as VS Code tasks.
- A project's `tests/` appear in the **Testing** view.

In a workspace you have not trusted, analysis still works but Run, Build, Check and
Test are disabled, because they would compile and run the workspace's code.

## Commands

| Command | |
|---|---|
| Turf: Run, Build, Check, Run Tests | Run `turf` on the project or file (needs the toolchain). |
| Turf: Restart Language Server | Restart the server. Changing `turf.server.path` or `turf.home` does this for you. |
| Turf: Show Language Server Log | Open the server's log. |
| Turf: Show Status | What is running, where it came from, and which standard library it found. |
| Turf: How to Install the Turf Toolchain | Open the installation instructions. |

## Settings

| Setting | Default | |
|---|---|---|
| `turf.toolchain.path` | empty | The `turf` tool, or its installation directory, for Run, Build, Check and Test. Empty looks on `PATH` and in `~/.turf`. |
| `turf.server.path` | empty | Use a different `turf-lsp` instead of the bundled one. |
| `turf.home` | empty | Take the standard library from a different Turf installation. |
| `turf.diagnostics.enable` | on | Show the language server's errors and warnings. |
| `turf.completion.snippets` | on | Offer completions with tab stops, such as call arguments. |
| `turf.format.onSave` | off | Format on save. The standard `editor.formatOnSave` for `[turf]` works too. |
| `turf.codeLens.enable` | on | The Run and Run Tests links above `main` and in test files. |
| `turf.testing.enable` | on | The project's tests in the Testing view. |
| `turf.trace.server` | off | Log the protocol traffic, for bug reports. |

## Troubleshooting

Click **Turf** in the status bar. It shows whether the language server is running,
where it came from and which standard library it found, and has *Show language
server log*. The log is also in **Output -> Turf Language Server**.

- **"the standard library was not found"**: reinstall the extension; it contains
  `server/lib/std`.
- **Run does nothing, or asks for the toolchain**: install the Turf toolchain, or set
  `turf.toolchain.path`.
- **Imports of your own modules are red**: they need a `turf.toml` in the project
  root (`turf new` makes one). A `turf.toml` that does not parse is reported on the
  file.

## Where it works

The extension uses only the standard VS Code API, so it also installs in VS Code
forks (VSCodium, Cursor, Windsurf and others) from Open VSX. It runs where your files
are, so Remote - SSH, WSL and dev containers work. It needs a folder on disk, so it
does not run in the browser (vscode.dev).

## Links

- [Source and issues](https://github.com/turf-lang/turf-lsp)
- [The Turf language](https://github.com/turf-lang/turf-lang)
- [Changelog](https://github.com/turf-lang/turf-lsp/blob/master/editors/vscode/CHANGELOG.md)
- Other editors: [Neovim, Emacs and Sublime Text setup](https://github.com/turf-lang/turf-lsp#other-editors)
