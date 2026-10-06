# Turf Language Server (`turf-lsp`)

The language server for the [Turf programming language](https://github.com/turf-lang/turf-lang),
speaking the Language Server Protocol 3.17 over stdio. It links the compiler's
front end in-process (`turfcore`, through the compiler's query facade
`turf::analyze()`), so every answer -- diagnostics, hover, completion,
definitions -- comes from the real compiler, not a re-implementation of it.

## Use it

**In VS Code or any fork (VSCodium, Cursor, Windsurf, ...): install the Turf
extension and open a `.tr` file.** That is all. The extension carries the
language server and the standard library for your platform (Linux x64, Windows
x64, macOS Apple silicon), so there is nothing else to install, build or
configure -- completion, imports, diagnostics, hover, go to definition,
formatting and the rest work at once, in single files and in projects
(`turf.toml`). See [editors/vscode/README.md](editors/vscode/README.md) for what
the extension does and its settings.

Running a program needs the Turf compiler (the *toolchain*); editing does not.

**Other editors** (Neovim, Emacs, Sublime, Helix, Zed, ...) use the same server:
[editors/neovim](editors/neovim/README.md), [editors/emacs](editors/emacs/README.md),
[editors/sublime](editors/sublime/README.md). The server is a single executable
that finds its standard library next to itself, so take it from a release (or
from the extension's `server/` directory) and keep its layout:

```
<anywhere>/bin/turf-lsp          the server
<anywhere>/bin/turf              the project helper (optional: needed for turf.toml projects)
<anywhere>/lib/std/...           the standard library
```

and point your editor at `<anywhere>/bin/turf-lsp`. `turf-lsp --version` prints
what it found:

```
turf-lsp 0.1.0 (LSP protocol 3.17)
compiler d54a43c5b6f0
stdlib /opt/turf (next to the server (../lib/std))
```

If the standard library is not found the server stays up and tells the editor
(a message, and `E027` on every `import`), rather than failing silently.

## What it does

| | |
|---|---|
| **Diagnostics** | errors and warnings as you type (with the compiler's error codes and hints, and the lint warnings `turf` prints), each underlined across the whole token |
| **Completion** | what is in scope at the cursor; `module::` members (only `pub` ones); `instance.` members; module names after `import`; keywords and templates for where you are (top level, function body, struct body); **type the name of a module you have not imported and accepting it adds the `import`** |
| **Signature help** | a call's parameters and the one being typed, for functions, module functions, static and instance methods, and every overload |
| **Hover**, **go to definition / declaration / type definition**, **references**, **document highlight**, **rename** (refuses the standard library) | |
| **Outline**, **workspace symbol search** (Ctrl+T, across files nobody has open), **folding**, **document links** (Ctrl+click an `import`), **semantic highlighting** | |
| **Inlay hints** | the parameter name before each argument (`add(a: 1, b: 2)`), except where the argument already says it |
| **Quick fixes** | add a missing `import`, organize imports, prefix an unused variable with `_` |
| **Formatting** | the document or a selection, only the lines that differ |
| **Projects** | inside a `turf.toml` project, `import` resolves as in `turf build`: your modules anywhere under `src/`, dependencies, `tests/`; unsaved edits to one file are seen by the files that import it; a manifest that does not parse is reported on the file |

Not done, on purpose: call and type hierarchy, a debug adapter (gdb works on
`-g` builds), and inferred-type inlay hints (Turf declarations spell their types).

## How it fits together

- `src/CompilerService.cpp` runs one analysis through `turf::analyze()`
  (`include/turf/Analysis.h` in the compiler): a project's package graph, the
  other open documents' text, cancellation, lint. Everything the compiler keeps in
  process-wide globals is saved and restored there, in one place.
- `src/SemanticQuery.cpp` answers questions about an analysis (completion,
  hover, tokens, signature help).
- `src/TextFeatures.cpp`, `src/Uri.cpp`, `src/StdlibLocator.cpp`,
  `src/ProjectGraph.cpp`: the parts that need no compiler -- text diffs,
  folding, `file://` URIs (including Windows and UNC), finding the standard
  library, asking `turf graph` for a project's packages.
- `src/LSPHandler.cpp` is the protocol: a thread for I/O and one worker that
  owns all compiler access.
- [ARCHITECTURE.md](ARCHITECTURE.md), [DEVELOPING.md](DEVELOPING.md) and [plan/](plan/)
  have the rest.

## Build from source

You only need this to work on the server. It builds against a checkout of the
compiler:

```bash
git clone https://github.com/turf-lang/turf-lang.git        # next to this repo, or anywhere
cmake -S . -B build -DTURF_LANG_DIR=../turf-lang -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
build/turf-lsp --version
```

Needs CMake 3.20+, a C++20 compiler and LLVM 22 (`find_package(LLVM CONFIG)`;
with Homebrew `-DCMAKE_PREFIX_PATH=$(brew --prefix llvm)`). The compiler's
`--smart` model runtime is not built. A default build links the shared LLVM and
so runs only where LLVM is installed; `-DTURF_STATIC_LLVM=ON` links LLVM's
static libraries, which is how the binaries in the extension are made (it needs
an LLVM that ships them, e.g. Debian/Ubuntu's `llvm-22-dev`, MSYS2's and
Homebrew's).

Run the server against a stdlib from the checkout: `TURF_HOME=../turf-lang build/turf-lsp`.

## Test

```bash
build/turf-lsp-tests                                              # unit tests: pure functions, no compiler run
python3 scripts/smoke.py --stdlib-from ../turf-lang               # the real server over stdio, as an editor drives it
python3 scripts/check_grammar.py                                  # the VS Code grammar covers every reserved word
cd editors/vscode && npm ci && npm test                           # extension unit tests
```

`scripts/smoke.py --stdlib-from DIR` stages an install (`bin/` + `lib/std`) with
`TURF_HOME` unset, which is the "install it and it works" case; pass
`--turf <path to turf>` (default: `DIR/build/turf`) to include the project tests.

The end-to-end test installs the **packaged** extension into a real VS Code and
drives it (on Linux it needs a display; `xvfb-run -a` in CI):

```bash
python3 scripts/package_vsix.py --server build/turf-lsp --turf ../turf-lang/build/turf --stdlib ../turf-lang
cd editors/vscode && node out/test/integration/runTest.js ../../dist/turf-<target>-0.1.0.vsix
```

`package_vsix.py` refuses a server that links libraries a user's machine may not
have; `--allow-dynamic` packages one anyway, for trying it on the machine that
built it. CI (`.github/workflows/build.yml`) does all of this on Linux, Windows
and macOS; `release.yml` publishes a tagged version.

## Troubleshooting

Click **Turf** in the VS Code status bar for what is running and the log.

| Symptom | Cause / fix |
|---|---|
| "the standard library was not found" | The server could not find `lib/std` next to itself, in `TURF_HOME`, or in `~/.turf`. Reinstall the extension; for another editor keep the `bin/` + `lib/` layout above. |
| Imports of your own modules are red | Put a `turf.toml` at the project root (`turf new` makes one); a broken one is reported on the file. |
| `turf-lsp` exits at once on Linux | A dev build links the shared LLVM: build with `-DTURF_STATIC_LLVM=ON`, or install LLVM 22. |
| Nothing happens in VS Code | **Output -> Turf Language Server**, and `turf.trace.server: "verbose"` for the protocol traffic. |
