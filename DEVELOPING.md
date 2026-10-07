# Developing `turf-lsp`

## Build and test

```bash
git clone https://github.com/turf-lang/turf-lang.git ../turf-lang   # or any checkout; pass -DTURF_LANG_DIR=...
cmake -S . -B build -DTURF_LANG_DIR=../turf-lang -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

build/turf-lsp-tests                                       # unit tests
python3 scripts/smoke.py --stdlib-from ../turf-lang        # the server over stdio, like an editor
```

The README has the rest (static linking, packaging the extension, the VS Code
integration test). Keep all of them green; CI runs them on Linux, Windows and
macOS.

## Build the VS Code extension on Windows (from local checkouts)

The extension is one `.vsix` per platform, and the Windows one has to be built
on Windows: the server inside it is a fully static `turf-lsp.exe` (LLVM linked
in, no DLLs next to it), made with MSYS2's MinGW toolchain exactly as the
`windows-x64` job in `.github/workflows/build.yml` does. These steps use your
local checkouts, so nothing needs to be pushed first.

**1. Install the tools once** (Windows 10/11, x64):

- [MSYS2](https://www.msys2.org) (installs to `C:\msys64`), [Go](https://go.dev/dl/)
  (for `turf.exe`; `turf-lang/cli/go.mod` says which version), [Node.js 20](https://nodejs.org),
  [Python 3](https://www.python.org/downloads/) and Git, and VS Code (its `code`
  command on `PATH`).
- Open **MSYS2 MINGW64** with the Windows `PATH` kept (so it sees Go and Node):
  `C:\msys64\msys2_shell.cmd -mingw64 -use-full-path`. Everything below runs in
  that shell.
- Install the build tools, then **check the LLVM version**:

  ```bash
  pacman -S --needed mingw-w64-x86_64-llvm mingw-w64-x86_64-clang mingw-w64-x86_64-gcc \
      mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-zlib \
      mingw-w64-x86_64-zstd mingw-w64-x86_64-python
  llvm-config --version        # must print 22.x
  ```

  The compiler targets **LLVM 22**. MSYS2 shipped 22.1.8 when this was written;
  when MSYS2 moves to 23 the compiler does not build (three API removals,
  `CreateGlobalStringPtr`, `OptimizationLevel::Os`, and `getTerminator()` now
  asserting), and you would need MSYS2's versioned `mingw-w64-x86_64-llvm-22`
  package if it offers one, or a port. Do not run `pacman -Syu` on a working
  setup right before a release build without checking this line again.

**2. Put the two repositories side by side** (the compiler's directory name does
not matter; below it is `turflang`):

```
C:\src\turflang       the compiler  (this branch, with the changes you want in the extension)
C:\src\turf-lsp       this repository
```

In the MSYS2 shell: `cd /c/src/turf-lsp`.

**3. Build `turf.exe`, the server, and package** (about 10-20 minutes; the
server step compiles the whole compiler front end):

```bash
# the project helper the server asks for a project's packages -- Go, static
(cd ../turflang/cli && mkdir -p ../../turf-lsp/out && \
   CGO_ENABLED=0 go build -trimpath -o ../../turf-lsp/out/turf.exe ./cmd/turf)

# the server, fully static
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DTURF_LANG_DIR=../turflang -DTURF_STATIC_LLVM=ON \
      -DCMAKE_EXE_LINKER_FLAGS="-static"
cmake --build build --parallel

# before packaging: the unit tests and the end-to-end test over stdio
build/turf-lsp-tests
python scripts/smoke.py --server build/turf-lsp.exe --stdlib-from ../turflang --turf out/turf.exe

# the extension: runs `npm ci`, compiles the TypeScript, bundles server + turf.exe
# + the standard library, and refuses a server that needs DLLs the user may lack
python scripts/package_vsix.py --target win32-x64 --server build/turf-lsp.exe \
       --turf out/turf.exe --stdlib ../turflang --out dist
```

The result is `dist/turf-win32-x64-<version>.vsix`.

**4. Install and try it** (from any Windows terminal; or in VS Code:
Extensions view, `...` menu, *Install from VSIX...*):

```
code --install-extension C:\src\turf-lsp\dist\turf-win32-x64-0.1.0.vsix
```

Open a folder, create `hello.tr`, and check, in this order:

1. `printline("hi");` inside `fn main(): int { ... }` shows **no** error and no
   suggestion to import `io` (the prelude needs none).
2. `mut l: list::List<int> = list::List::default();` shows `E145`, and `Ctrl+.`
   offers **Add `import list;`**.
3. `json::parse("1");` shows `E031` and `Ctrl+.` offers **Add `import json;`**.
4. Hover and **F12** on `printline` land in the bundled `lib/std/core/io.tr`.
5. Click **Turf** in the status bar: it shows what is running and the log. The
   server and the standard library should be the ones inside the extension's
   folder, not something found through a `TURF_HOME` or `~/.turf` you had.

If a step fails: **Output -> Turf Language Server**, and set
`turf.trace.server` to `verbose`. A server that dies at once on a clean machine
nearly always means it was not static (`package_vsix.py` checks this, so use no
`--allow-dynamic`); a "standard library not found" message means `lib/std` did
not make it into the package.

To try the *same* build without installing it into your daily VS Code, add
`--user-data-dir C:\temp\vsc-user --extensions-dir C:\temp\vsc-ext` to the
`code --install-extension` command and to the `code` that opens your folder.

## Adding a feature

1. **Decide what it needs.** If it is a function of the text alone (folding,
   import statements, a diff), put it in `TextFeatures` with a unit test: it then
   works on a half-typed buffer and needs no compiler. If it needs to know what
   a name means, it is a question about an analysis: `SemanticQuery`.
2. **Advertise it** in the `initialize` response (`LSPHandler::handleInitialize`),
   **dispatch** the method (`LSPHandler::dispatch`), and write `handleX` (parse
   the request on the I/O thread, register it as cancellable, enqueue) and `doX`
   (runs on the worker; check the cancel flag; answer once).
3. **Test it** in `scripts/smoke.py`: a test function `t_<name>(c, d)` that asks
   for the feature over stdio and asserts on the answer. A test that cannot fail
   proves nothing: break the feature on purpose once and see it fail.
4. If the VS Code extension should expose it (a setting, a command), add that in
   `editors/vscode` and say so in its README; the extension must not offer
   anything the server does not do.

## Things that have gone wrong before

- **Positions.** The compiler counts bytes from 1; LSP counts UTF-16 units (or
  UTF-8) from 0. Always go through `PositionMapper`.
- **Squiggle widths** come from the document's text (`CompilerService.cpp`), not
  from any compiler global: the facade restores those, and a width read from them
  silently becomes one byte.
- **Module names.** The AST names a dependency's module `dep/dep`; what is on the
  screen is `dep`. Measure and print `spelledModule()`.
- **URIs.** Never build `"file://" + path`; use `turf_lsp::pathToUri()` (spaces,
  accents, Windows drives).
- **Windows stdio** must be binary (`main.cpp`), or `Content-Length` is wrong.
- **A server that exits** is the worst failure: nothing is left to tell the user.
  The compiler raises errors that call `exit(1)` outside a recovery guard; the
  facade guards the places that matter. If you find another, fix it in the facade
  and add a test to the compiler's `tests/api`.

## Debugging

stdout is the protocol: never print to it. Log with `Logger` (stderr, and
`TURF_LSP_LOG=<file>`, `TURF_LSP_LOG_LEVEL=debug`). In VS Code, **Output -> Turf
Language Server**, and `turf.trace.server: verbose` for the traffic.
