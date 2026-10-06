# Turf for Visual Studio Code

Install it, open a `.tr` file, and start writing Turf. There is nothing else to
install and nothing to configure: the language server and the standard library
come with the extension.

## What you get

- **Diagnostics** as you type, with the compiler's own explanations and error
  codes; warnings (unused variables, unreachable code) too.
- **Completion**: variables and functions in scope, struct fields and methods,
  `module::` members (only what a module makes `pub`), module names after
  `import`, keywords and templates for the statement you are on. Type the name
  of a module you have not imported and accepting it **adds the `import`**.
- **Signature help** while typing a call's arguments.
- **Inlay hints**: the parameter name before each argument (`add(a: 1, b: 2)`).
- **Hover**, **go to definition** (including into the standard library and into
  your own modules), **find references**, **rename**, **document highlights**,
  **outline**, **workspace symbol search** (Ctrl+T) and **document links**
  (Ctrl+click an `import`).
- **Quick fixes** (`Ctrl+.`): add a missing `import`, organize imports, and the
  fixes the compiler's errors suggest.
- **Formatting** of the document or a selection, changing only the lines that
  differ.
- **Folding**, **semantic highlighting**, and a status-bar item that tells you
  what is running.
- **Projects**: in a folder with `turf.toml`, imports resolve exactly as in
  `turf build` -- your own modules anywhere under `src/`, and your dependencies.
  Unsaved edits to one file are seen by the files that import it.

## Running programs

Editing needs nothing else. **Running** a program needs the Turf compiler:

- **Turf: Run**, **Build**, **Check** and **Run Tests** (command palette, the Run
  button, or the links above `main` and in test files), as tasks.
- Your project's `tests/` appear in the **Testing** view.

If the compiler is not installed, these tell you so and show how to get it;
everything above keeps working. If it is installed somewhere unusual, set
`turf.toolchain.path`.

## Settings

| Setting | |
|---|---|
| `turf.toolchain.path` | The `turf` tool (or its installation directory), for Run/Build/Check/Test. |
| `turf.server.path` | Use a different `turf-lsp` instead of the bundled one. |
| `turf.home` | Take the standard library from a different Turf installation. |
| `turf.diagnostics.enable` | Turn the language server's errors and warnings off. |
| `turf.completion.snippets` | Completions with tab stops. |
| `turf.format.onSave` | Format on save (or use `editor.formatOnSave` for `[turf]`). |
| `turf.codeLens.enable`, `turf.testing.enable` | The Run links and the Testing view. |
| `turf.trace.server` | Log the protocol traffic (for bug reports). |

## If something is wrong

Click **Turf** in the status bar: it shows whether the language server is
running, where it came from, and which standard library it found, and has
*Show language server log*. The log is also in **Output → Turf Language Server**.

- **"the standard library was not found"**: reinstall the extension; it should
  contain `server/lib/std`.
- **Run does nothing / asks for the toolchain**: install the Turf toolchain, or
  set `turf.toolchain.path`.
- **Imports of your own modules are red**: they need a `turf.toml` in the project
  root (`turf new` makes one); a broken `turf.toml` is reported on the file.

## Where it works

The extension is published per platform (Linux x64, Windows x64, macOS Apple
silicon) and uses only the standard VS Code extension API, so it also installs
in VS Code forks (VSCodium, Cursor, Windsurf, ...) from Open VSX. It runs where
your files are, so Remote - SSH, WSL and dev containers work. It needs a real
folder on disk, so it does not run in the browser (vscode.dev).

In a workspace you have not trusted, analysis works but Run, Build and Test are
disabled.
