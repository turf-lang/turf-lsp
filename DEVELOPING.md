# Developing turf-lsp

How the pieces fit is in [ARCHITECTURE.md](ARCHITECTURE.md). This page is about
building, testing, changing and releasing.

## What you need

- CMake 3.20 or later, a C++20 compiler, Python 3, and LLVM 22
  (`find_package(LLVM CONFIG)`; with Homebrew, `-DCMAKE_PREFIX_PATH=$(brew --prefix llvm)`).
  The compiler targets LLVM 22 exactly.
- A checkout of the compiler, [turf-lang/turf-lang](https://github.com/turf-lang/turf-lang).
  It is built from source as part of this project (`add_subdirectory`, only the
  `turfcore` target), so a compiler change is a server change. The default location
  is `../turf-lang`; anything else is `-DTURF_LANG_DIR=<path>`. The compiler's
  `--smart` model runtime is not built.
- `turf`, the command-line tool, for project support and the project tests. It is
  Go and comes from [turf-lang/turf-cli](https://github.com/turf-lang/turf-cli):
  `CGO_ENABLED=0 go build -trimpath -o out/turf ./cmd/turf`. The compiler pins the
  revision it is tested with in its `TURF_CLI_REVISION` file.
- Node.js 20, for the extension.

## Build

```bash
git clone https://github.com/turf-lang/turf-lang.git ../turf-lang
cmake -S . -B build -DTURF_LANG_DIR=../turf-lang -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
build/turf-lsp --version
```

A default build links the shared LLVM and so runs only where LLVM is installed.
`-DTURF_STATIC_LLVM=ON` links LLVM's static libraries, which is how the shipped
binaries are made. It needs an LLVM that ships them (Debian and Ubuntu's
`llvm-22-dev`, MSYS2's and Homebrew's do). To run a development server against a
standard library from the checkout: `TURF_HOME=../turf-lang build/turf-lsp`.

## Test

```bash
build/turf-lsp-tests                                          # pure functions, no compiler run
python3 scripts/smoke.py --stdlib-from ../turf-lang --turf out/turf   # the real server over stdio
python3 scripts/check_grammar.py                              # the TextMate grammar covers every reserved word
cd editors/vscode && npm ci && npm test                       # extension unit tests
```

`smoke.py` stages an install (`bin/` plus `lib/std`) from `--stdlib-from` with
`TURF_HOME` unset, which is the "install it and it works" case. Without `--turf`
the tests that need project support are skipped. `check_grammar.py` compares
`syntaxes/turf.tmLanguage.json` with the keyword table of the built server
(`turf-lsp --print-keywords`).

The end-to-end test installs the packaged extension into a real VS Code and drives
it. On Linux it needs a display (`xvfb-run -a`):

```bash
python3 scripts/package_vsix.py --server build/turf-lsp --turf out/turf --stdlib ../turf-lang
cd editors/vscode && node out/test/integration/runTest.js ../../dist/turf-<target>-<version>.vsix
```

`package_vsix.py` refuses a server that links libraries a user's machine may not
have. `--allow-dynamic` packages one anyway, for trying it on the machine that built
it. It writes the `.vsix` and the standalone server archive
`turf-lsp-<target>-<version>.tar.gz` (`.zip` on Windows) to `dist/`.

## Continuous integration

- `build.yml` runs on every push and pull request, on Linux x64, Windows x64 and
  macOS arm64. Each job builds `turf` and a statically linked server against the
  compiler release in `TURF_LANG_REF` (`v2.0.0`; a release ships against a
  released compiler, never its `master`), with `turf` at the turf-cli revision
  that release pins, runs the unit tests, the grammar check and the smoke test,
  packages the extension, runs the extension's unit tests, and installs the
  packaged `.vsix` into a real VS Code.
- `compiler-sync.yml` runs every night at 00:00 IST against the compiler's
  `master`, and when the compiler dispatches `compiler-updated`. It runs the unit
  tests, the grammar check and the smoke test on Linux, without packaging. The
  compiler changes on its own schedule, and a change to `include/turf/Analysis.h`,
  the prelude, the keyword table or the standard library's names would otherwise
  show up as a red build on an unrelated push here. A failed scheduled run opens
  the issue "Compiler sync broken" (label `compiler-sync`) naming the failed steps
  and the compiler commit; the next green run closes it.
- `release.yml` is below.

## Releasing

1. Set the version in `CMakeLists.txt` (`project(... VERSION x.y.z)`; it is what
   `--version` and `serverInfo.version` report), in `editors/vscode/package.json`
   and `package-lock.json`, and in `turf.minServerVersion` in `package.json` if the
   extension needs a newer server than before. Update the release tag in
   `editors/neovim/upstream/mason-registry/packages/turf-lsp/package.yaml`, and add
   the entry to `editors/vscode/CHANGELOG.md`.
2. Push a tag `vX.Y.Z`. `release.yml` refuses a tag that differs from the
   extension's version, runs the whole of `build.yml`, then:
   - attaches the three `.vsix` files and the server archives
     `turf-lsp-<target>-<version>` to a GitHub release;
   - publishes the `.vsix` files to the Visual Studio Marketplace if the
     repository secret `VSCE_PAT` is set, and to Open VSX if `OVSX_PAT` is set
     (creating the publisher's namespace there if it does not exist). A registry
     without its token is skipped, not failed.

The targets are `linux-x64`, `win32-x64` and `darwin-arm64`.

## Adding a feature

1. **Decide what it needs.** If it is a function of the text alone (folding,
   import statements, a diff), put it in `TextFeatures` with a unit test: it then
   works on a half-typed buffer and needs no compiler. If it needs to know what a
   name means, it is a question about an analysis: `SemanticQuery`.
2. **Advertise it** in the `initialize` response (`LSPHandler::handleInitialize`),
   **dispatch** the method (`LSPHandler::dispatch`), and write `handleX` (parse the
   request on the I/O thread, register it as cancellable, enqueue) and `doX` (runs
   on the worker; check the cancel flag; answer once).
3. **Test it** in `scripts/smoke.py`: a function `t_<name>(c, d)` that asks for the
   feature over stdio and asserts on the answer. A test that cannot fail proves
   nothing, so break the feature on purpose once and watch it fail.
4. If the VS Code extension should expose it (a setting, a command), add that in
   `editors/vscode` and in its README. The extension must not offer anything the
   server does not do.

## Things that have gone wrong before

- **Positions.** The compiler counts bytes from 1; LSP counts UTF-16 units (or
  UTF-8) from 0. Always go through `PositionMapper`.
- **Squiggle widths** come from the document's text (`CompilerService.cpp`), not
  from a compiler global: the facade restores those, and a width read from them
  silently becomes one byte.
- **Module names.** The AST names a dependency's module `dep/dep`; what is on the
  screen is `dep`. Measure and print `spelledModule()`.
- **URIs.** Never build `"file://" + path`; use `turf_lsp::pathToUri()` (spaces,
  accents, Windows drives).
- **Windows.** Stdio must be binary (`main.cpp`), or `Content-Length` is wrong. Both
  executables embed a manifest that makes UTF-8 the process code page
  (`src/windows/`), because the server and the compiler hold paths as UTF-8
  strings and every narrow-string file API otherwise reads them as the ANSI code
  page.
- **A server that exits** is the worst failure: nothing is left to tell the user.
  The compiler raises errors that call `exit(1)` outside a recovery guard; the
  facade guards the places that matter. If you find another, fix it in the facade
  and add a test to the compiler's `tests/api`.
- **A prelude module needs no import.** `io` is one of the compiler's nine prelude
  modules, so `io::printline` without an import is not an error. Use a non-prelude
  module such as `json` when a test needs a missing import.

## Debugging

stdout is the protocol: never print to it. Log with `Logger`, which writes to
stderr and, with `TURF_LSP_LOG=<file>`, to that file as well
(`TURF_LSP_LOG_LEVEL=debug` for detail). In VS Code, click **Turf** in the status
bar, or open **Output -> Turf Language Server**; set `turf.trace.server` to
`verbose` for the protocol traffic.

## Building the Windows extension from local checkouts

The Windows `.vsix` has to be built on Windows: the server in it is a fully static
`turf-lsp.exe` (LLVM linked in, no DLLs beside it), made with MSYS2's MinGW
toolchain as the `windows-x64` job of `build.yml` does. These steps use local
checkouts, so nothing needs to be pushed.

**1. Install the tools once** (Windows 10 1903 or later, x64): [MSYS2](https://www.msys2.org),
[Go](https://go.dev/dl/), [Node.js 20](https://nodejs.org), Python 3, Git, and VS
Code with `code` on `PATH`. Open **MSYS2 MINGW64** with the Windows `PATH` kept, so
it sees Go and Node: `C:\msys64\msys2_shell.cmd -mingw64 -use-full-path`. Everything
below runs in that shell. Then:

```bash
pacman -S --needed mingw-w64-x86_64-llvm mingw-w64-x86_64-clang mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-zlib \
    mingw-w64-x86_64-zstd mingw-w64-x86_64-python
llvm-config --version        # must print 22.x
```

The compiler needs LLVM 22. If MSYS2 has moved to a later LLVM, the compiler does
not build against it; use MSYS2's versioned `mingw-w64-x86_64-llvm-22` package if
there is one. Check this line before a release build instead of running
`pacman -Syu` blind.

**2. Put the repositories side by side**: `turf-lang` (the compiler), `turf-cli`
and `turf-lsp`. In the shell, `cd` into `turf-lsp`.

**3. Build and package** (10 to 20 minutes; the server step compiles the whole
compiler front end):

```bash
# turf, the project helper: Go, static
(cd ../turf-cli && CGO_ENABLED=0 go build -trimpath -o ../turf-lsp/out/turf.exe ./cmd/turf)

# the server, fully static
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DTURF_LANG_DIR=../turf-lang -DTURF_STATIC_LLVM=ON \
      -DCMAKE_EXE_LINKER_FLAGS="-static"
cmake --build build --parallel

build/turf-lsp-tests
python scripts/smoke.py --server build/turf-lsp.exe --stdlib-from ../turf-lang --turf out/turf.exe

# runs `npm ci`, compiles the TypeScript, bundles server + turf.exe + the standard library
python scripts/package_vsix.py --target win32-x64 --server build/turf-lsp.exe \
       --turf out/turf.exe --stdlib ../turf-lang --out dist
```

The result is `dist/turf-win32-x64-<version>.vsix`.

**4. Try it.** `code --install-extension dist\turf-win32-x64-<version>.vsix`, or in
VS Code the Extensions view, `...` menu, *Install from VSIX...*. To keep it away
from your daily VS Code, add `--user-data-dir` and `--extensions-dir` pointing at
scratch folders to that command and to the `code` that opens your test folder. Open
a folder, create `hello.tr` and check:

1. `printline("hi");` inside `fn main(): int { ... }` has no error and no
   suggestion to import `io`.
2. `json::parse("1");` is an error, and `Ctrl+.` offers **Add `import json;`**.
3. Hover and **F12** on `printline` land in the bundled `lib/std/core/io.tr`.
4. The **Turf** status bar item shows the server and standard library inside the
   extension's folder, not ones found through a `TURF_HOME` or `~/.turf` you have.

If a step fails, read the log (see Debugging). A server that dies at once on a clean
machine nearly always was not static (`package_vsix.py` checks this, so do not pass
`--allow-dynamic`); "standard library not found" means `lib/std` did not make it
into the package.
