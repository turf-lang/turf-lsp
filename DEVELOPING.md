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
