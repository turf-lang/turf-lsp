# Turf Language Server (`turf-lsp`)

The official Language Server Protocol (LSP 3.17) implementation for the
[Turf programming language](../turf-lang/README.md). Written in C++,
links the compiler (`turfcore`) directly in-process — no shelling out, no
FFI — so every answer it gives (diagnostics, hover, completion, go-to-def,
...) comes from the real compiler, not a re-implementation of it.

This README is the practical "build it, run it, try it in VS Code" guide.
For internals see [ARCHITECTURE.md](ARCHITECTURE.md); for contributing a
new LSP feature see [DEVELOPING.md](DEVELOPING.md); for the full
feature-by-feature implementation plan see [plan/](plan/).

## Table of contents

1. [Prerequisites](#prerequisites)
2. [Building](#building)
3. [Try it in VS Code](#try-it-in-vs-code)
4. [VS Code settings](#vs-code-settings)
5. [Features, and how to trigger each one](#features-and-how-to-trigger-each-one)
6. [`turf-lsp` CLI flags](#turf-lsp-cli-flags)
7. [`turf` compiler CLI flags](#turf-compiler-cli-flags)
8. [Use cases / examples](#use-cases--examples)
9. [Testing](#testing)
10. [Other editors](#other-editors)
11. [Troubleshooting](#troubleshooting)

## Prerequisites

- **This repo's layout matters**: `turf-lsp/CMakeLists.txt` pulls the
  compiler in via `add_subdirectory(../turf-lang ...)` — `turf-lang/` and
  `turf-lsp/` must be sibling directories on disk (which they already are
  in this checkout). You do **not** need to build `turf-lang` separately
  first; building `turf-lsp` builds `turfcore` (the compiler library) as
  part of the same CMake configure/build.
- **LLVM 22**, discoverable by CMake's `find_package(LLVM CONFIG)`. On
  macOS with Homebrew:
  ```bash
  brew install llvm@22
  export PATH="$(brew --prefix llvm@22)/bin:$PATH"   # add to ~/.zshrc to persist
  ```
  If CMake still can't find it, pass it explicitly:
  `-DCMAKE_PREFIX_PATH=$(brew --prefix llvm@22)`. On Linux, install LLVM
  22 via your distro's package manager or from source, and make sure
  `LLVMConfig.cmake` is on CMake's search path the same way.
- **CMake 3.20+** and a **C++20** compiler (Clang recommended; this is
  what the whole session's build used).
- **Node.js + npm**, only needed for the VS Code extension (`editors/vscode/`).
- The first `turf-lang` configure also builds its vendored `llama.cpp`
  submodule (used by the compiler's optional `--smart` diagnostics mode) —
  slow once, fast after. Not needed by the LSP itself, just unavoidable
  the first time `turfcore` is built.

## Building

### The language server (`turf-lsp`)

```bash
cd turf-lsp
mkdir -p build && cd build
cmake ..
cmake --build . -j 4          # or -j$(nproc) / -j$(sysctl -n hw.logicalcpu)
```

The binary lands at `turf-lsp/build/turf-lsp`. Verify it:

```bash
./turf-lsp --version
# turf-lsp 0.1.0 (LSP protocol 3.17)
```

Rebuild after any source change with `cmake --build . -j4` from inside
`build/` — no need to re-run the `cmake ..` configure step unless
`CMakeLists.txt` itself changed.

### The compiler CLI (`turf`), optional but recommended

You'll want this too, for compiling/running the example `.tr` programs
from a terminal alongside the editor:

```bash
cd turf-lang
./scripts/cmake_build.sh --release
```

Binary lands at `turf-lang/build/turf`. See
[`turf` compiler CLI flags](#turf-compiler-cli-flags) below.

## Try it in VS Code

Two ways to load the extension — pick based on whether you want to poke
at it once or actually use it day to day.

### Option A — Extension Development Host (fastest, no packaging)

```bash
cd turf-lsp/editors/vscode
npm install
npm run compile
```

Then in VS Code:
1. `File > Open Folder...` → open `turf-lsp/editors/vscode`.
2. Press **F5** (or Run > Start Debugging). A second VS Code window opens
   — titled `[Extension Development Host]` — with the extension loaded.
3. In *that* window, open a folder containing `.tr` files — e.g.
   `turf-lang/` itself, or `turf-code/` (the parent of both repos) so the
   binary auto-discovery (below) has the best shot at finding
   `turf-lsp/build/turf-lsp` automatically.
4. Open any `.tr` file (try `turf-lang/showcase/text_toolkit.tr`) and the
   server should attach within a second or two — check the status bar
   item in the bottom right (shows a spinner, then a checkmark once
   connected).

Re-run `npm run compile` (or `npm run watch` for auto-rebuild) after
editing `src/extension.ts`, then relaunch the Extension Development Host
(⌘R / Ctrl+R inside it, or stop and press F5 again) to pick up the change.

### Option B — Install into your real VS Code (persistent)

Package a `.vsix` and install it like any other extension:

```bash
cd turf-lsp/editors/vscode
npm install
npx @vscode/vsce package
# -> turf-vscode-0.1.0.vsix
# (vsce may warn about a missing "repository"/"license" field in
# package.json -- harmless, it still produces the .vsix)
```

Then either:
- In VS Code: Extensions view → `...` menu (top right) → **Install from
  VSIX...** → select `turf-vscode-0.1.0.vsix`, **or**
- From a terminal: `code --install-extension turf-vscode-0.1.0.vsix`

Reload VS Code, open any `.tr` file, and it just works the same as
Option A — no F5, no separate dev-host window.

### Either way: point it at the binary if auto-discovery misses

The extension looks for `turf-lsp` in this order: the `turf.server.path`
setting → a `build/turf-lsp` relative to your open workspace (several
common layouts, see [editors/vscode/README.md](editors/vscode/README.md))
→ next to the extension's own install location (dev mode only) → `turf-lsp`
on `PATH`. If it shows a warning that it couldn't find the binary, set it
explicitly in your workspace or user `settings.json`:

```json
{
  "turf.server.path": "/absolute/path/to/turf-lsp/build/turf-lsp"
}
```

## VS Code settings

| Setting | Default | What it does |
|---|---|---|
| `turf.server.path` | `""` (auto-discover) | Explicit path to the `turf-lsp` binary. |
| `turf.home` | `""` | Sets `TURF_HOME` in the server's environment — where it looks for the stdlib (`lib/std/`) and installed packages (`lib/usr/`). Leave empty to use the server binary's own default (its own location's `lib/`). |
| `turf.trace.server` | `"off"` | `"messages"` or `"verbose"` to log the raw JSON-RPC traffic to the output channel — useful when something isn't working and you want to see exactly what's being sent. |
| `turf.diagnostics.enable` | `true` | Toggle diagnostics. |
| `turf.format.onSave` | `false` | Run `textDocument/formatting` automatically whenever you save a `.tr` file. |
| `turf.completion.snippets` | `true` | Include tab-stop placeholders in call-argument completions. |

Changing `turf.server.path` or `turf.home` restarts the server
automatically. Otherwise, use the **Turf: Restart Language Server**
command (Cmd/Ctrl+Shift+P → type "Turf") or click the status bar item.

## Features, and how to trigger each one

| Feature | How to trigger | Notes |
|---|---|---|
| Diagnostics (errors/warnings) | Automatic — red/yellow squiggles as you type | Debounced; re-publishes ~250ms after you stop typing. |
| Syntax highlighting | Automatic | TextMate grammar (`syntaxes/turf.tmLanguage.json`) + semantic tokens from the server (more accurate — e.g. distinguishes a parameter from a local, a struct-field type from a plain identifier). |
| Hover | Mouse over an identifier, or Cmd/Ctrl+K Cmd/Ctrl+I | Shows the resolved type/signature; struct fields, enum members, module imports, and overload sets all supported. |
| Completion | Ctrl+Space, or automatically after typing `.` or `::` | Context-aware: bare-name, `Module::`, `instance.`, and `import <partial>`/`import mod::<partial>` all have distinct completion lists. |
| Go to Definition | F12, or Cmd/Ctrl+click | Cross-file, including into stdlib modules. |
| Go to Declaration | Cmd/Ctrl+F12 (or via the command palette) | |
| Go to Type Definition | Right-click → Go to Type Definition | |
| Find All References | Shift+F12 | Workspace-wide; scans every `.tr` file under the open folder(s). |
| Document Highlight | Automatic — click into an identifier, other occurrences in the same file get a subtle highlight | |
| Document Symbols / Outline | Ctrl+Shift+O, or the Outline panel in the sidebar | Flat list of functions/structs/enums/etc. in the current file. |
| Format Document | Shift+Alt+F, or automatically on save if `turf.format.onSave` is `true` | Indentation-only (doc 09) — never rewrites tokens, spacing, or line breaks, only fixes indentation to match brace/bracket nesting. Idempotent. |
| Rename Symbol | F2 | Workspace-wide for exported functions/types; file-local for local variables. Refuses to rename a keyword, a stdlib symbol, or anything without a real declaration. |
| Quick Fix / Code Actions | Click the 💡 lightbulb, or Ctrl+. (Cmd+.) | Two fixes currently shipped: **"Add `import x;`"** on an unresolved-module error, and **"Prefix unused `x` with `_`"** on an unused-variable warning (see the [known gap](#testing) note below — that specific warning doesn't reach the LSP's diagnostics yet, so this fix currently has nothing to attach to; it's correct and ready for when that's closed). |

## `turf-lsp` CLI flags

```
turf-lsp [options]
```

| Flag | Effect |
|---|---|
| (none) | Normal operation: reads/writes framed JSON-RPC over stdin/stdout. This is how an editor launches it — you won't see anything if you run it bare in a terminal, it's just waiting for a client to speak LSP at it (Ctrl+C to exit). |
| `-v`, `--version` | Print `turf-lsp <version> (LSP protocol 3.17)` and exit. Doesn't touch stdin or start the server — safe to run to sanity-check a build. |
| `-h`, `--help` | Print usage and exit. |

Environment: `TURF_HOME` overrides where the server resolves stdlib/
installed-package imports from (same variable the `turf` CLI itself
reads).

## `turf` compiler CLI flags

These are the underlying compiler's flags (`turf-lang/build/turf`), useful
when compiling/running the example programs below from a terminal:

```
turf <filename.tr> [-o output] [--emit-llvm] [--smart] [--dump-ast]
     [--resolve] [--dump-resolver-symbols] [--dump-resolved] [--typecheck]
     [--dump-typechecked] [--dump-modules] [--link <libname>]
turf --format [--write] <filename.tr>
turf --explain E0NN
```

| Flag | Effect |
|---|---|
| `-o <name>` | Output binary name (default derived from the input filename). |
| `--emit-llvm` | Emit LLVM IR instead of (or alongside) a native binary — useful for inspecting codegen. |
| `--smart` | Enable the AI-assisted diagnostics pipeline (local model via `llama.cpp`, no network call) — on a compile error, attempts a grammar-constrained, speculatively-tested fix suggestion. No-op on a clean compile. |
| `--dump-ast` | Print the parsed AST before any semantic analysis. |
| `--resolve` | Run just the Resolver pass (name/scope resolution) and stop. |
| `--dump-resolver-symbols` | Resolve, then dump the resolved symbol table. |
| `--dump-resolved` | Resolve, then print the AST annotated with resolution results. |
| `--typecheck` | Run Resolver + TypeChecker and stop (no codegen, no binary produced) — the fastest way to check "does this compile" without linking. |
| `--dump-typechecked` | Typecheck, then print the AST annotated with every node's inferred type. |
| `--dump-modules` | Resolve, then dump everything the compiler knows about the program's module graph (every imported module's functions, struct/enum ownership, on-demand imports). |
| `--link <lib>` | Link an additional native library (repeatable). |
| `--format [--write]` | Print the indentation-formatted source to stdout; with `--write`, rewrite the file in place instead. Same engine `textDocument/formatting` uses. |
| `--explain E0NN` | Print the full explanation for an error code shown in a `[E0NN]` diagnostic header. |

Example:
```bash
turf-lang/build/turf turf-lang/showcase/text_toolkit.tr -o text_toolkit
./text_toolkit
```

## Use cases / examples

The fastest way to see the language server doing real work is to open one
of these in VS Code (via either option above) — all live under
`turf-lang/showcase/`, all realistic, runnable programs, not toy snippets:

| File | What it demonstrates |
|---|---|
| `text_toolkit.tr` | String processing: `strings::split`/`pad_right`, `fs`'s path helpers, `collections::Set<int>`, timing an analysis. Good first file to open — pure stdlib usage, no networking/FFI, compiles clean (0 diagnostics) so you can see hover/completion/go-to-def without any error noise. |
| `config_and_dice.tr` | Builds a config value, round-trips it through `json::stringify`/`json::parse` (real text serialization, not just reusing the in-memory value), reads settings back out. |
| `http_server.tr` / `http_client.tr` | A real HTTP/1.1 server and a client that talks to it over an actual TCP socket, both written entirely in Turf using the `net` stdlib module — no `extern fn`, no raw pointers, no manual memory management visible at this layer. Run the server first, then the client, in separate terminals. |
| `kv_store_server.tr` / `kv_store_client.tr` | A miniature Redis-shaped key-value store over TCP, combining `net`, `list`, `strings`, and `mem` — closer to what a real small service looks like than the HTTP demo. |
| `llvm_ffi_selfhost_demo.tr` | Drives LLVM's C API directly via `extern fn` from Turf — builds a real LLVM context/module/function IR at runtime. Good for seeing `extern`/FFI-related hover and diagnostics. |

A minimal "hello world" to type from scratch and watch diagnostics/hover
update live:

```turf
import io;

fn main(): int {
    io::printline("Hello, Turf!");
    return 0;
}
```

Save it as `hello.tr`, open it in the extension-loaded VS Code window, and:
- Hover `printline` → see its resolved signature from the `io` module.
- Ctrl/Cmd+click `printline` → jumps into `lib/std/io/io.tr`.
- Delete the `;` → a diagnostic appears within ~250ms.
- Break the indentation, then Shift+Alt+F → it's restored.

To compile and run it from a terminal instead:
```bash
turf-lang/build/turf hello.tr -o hello && ./hello
```

`turf --explain E0NN` is worth trying on any error code you see in a
diagnostic's `[E0NN]` header (also shown as the diagnostic's `code` field
in VS Code's Problems panel) — it prints the full writeup for that error
class, not just the one-line message.

## Testing

**`turf-lang` (the compiler)** has a real regression suite:
```bash
cd turf-lang
./scripts/test_runner.sh
```
Runs all 606 tests under `tests/`. One test
(`tests/29_time_stdlib/02_sleep_wall_clock.tr`) is known-flaky under load
— it sleeps ~2.5s total and can race the 5s per-test timeout; a solo
re-run of just that file reliably passes. Everything else should be green.

**`turf-lsp` (the server)** does not yet have an automated test suite
(doc 12 in `plan/` specs one — SemanticQuery unit tests, protocol/
integration tests, editor smoke tests — none of it built yet). Until
then, verify manually:
1. Build and load the extension (either option above).
2. Open `turf-lang/showcase/text_toolkit.tr` → confirm 0 diagnostics
   (it's a clean file) and that syntax highlighting looks right.
3. Introduce a deliberate error (delete a `;`, misspell an import) →
   confirm a squiggle + Problems-panel entry with an `E0NN` code appears
   within about a second, and disappears again once fixed.
4. Try hover, F12 (definition), Shift+F12 (references), F2 (rename), and
   Shift+Alt+F (format) on a few different symbols.
5. Check `turf.trace.server: "verbose"` + the "Turf Language Server"
   output channel if anything seems stuck — it shows the raw JSON-RPC
   traffic and the server's own stderr logs.

**Known gap**: `CompilerService::analyze()` (what the LSP runs on every
edit) never runs the CFG-based lint pass that produces unused-variable/
dead-store warnings — that pass lives in Codegen, a phase the LSP
intentionally skips for speed. Those warnings show up under
`turf --typecheck` on the real CLI but not yet in the editor. See
`plan/03-diagnostics.md`'s "Known gap" section.

## Other editors

- **Neovim**: see [editors/neovim/README.md](editors/neovim/README.md) —
  both an `nvim-lspconfig` wrapper and a native Neovim 0.11+
  `vim.lsp.config` path.
- **Emacs**: [editors/emacs/README.md](editors/emacs/README.md) (`eglot`/`lsp-mode`).
- **Sublime Text**: [editors/sublime/README.md](editors/sublime/README.md) (`LSP` package).
- Any other LSP 3.17 client should work against the raw `turf-lsp`
  binary — it's editor-agnostic stdio JSON-RPC.

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `cmake ..` fails to find LLVM | `find_package(LLVM REQUIRED CONFIG)` can't locate `LLVMConfig.cmake`. Pass `-DCMAKE_PREFIX_PATH=$(brew --prefix llvm@22)` (macOS) or point at your LLVM 22 install's cmake dir directly. |
| VS Code shows "couldn't locate the turf-lsp binary" | Set `turf.server.path` explicitly (see [above](#either-way-point-it-at-the-binary-if-auto-discovery-misses)), or open a workspace root where auto-discovery's candidate paths actually match (see [editors/vscode/README.md](editors/vscode/README.md)). |
| No diagnostics/hover/etc. at all, status bar stuck spinning | Set `turf.trace.server: "verbose"`, reload, reproduce, then check the "Turf Language Server" output channel for the server's stderr — a crash-on-launch or a mid-session exception will show up there. |
| Imports to stdlib modules (`io`, `net`, ...) don't resolve | The server derives its stdlib search root the same way the `turf` CLI does (`TURF_HOME`, or `lib/` next to its own binary). If you moved the `turf-lsp` binary somewhere without `lib/` alongside it, set `turf.home` explicitly. |
| Extension compiles but F5 does nothing / errors | Make sure you ran `npm install && npm run compile` inside `editors/vscode/` first, and that you opened `editors/vscode/` itself (not a parent folder) as the workspace before pressing F5. |
