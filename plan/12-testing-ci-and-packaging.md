# 12 — Testing, CI, and packaging

## Goal
A test strategy that keeps the server correct as features land, plus CI and
release mechanics for the server binary and the editor extensions. Mirrors the
rigor of `turf-lang/AGENTS.md` (every fix gets a regression test; suite stays
green).

## Test layers

### 1. Compiler-facade unit tests (in `turf-lang`)
For the doc 02 additions (`turf::query::analyze`, `resolveModuleFile`,
`symbolId()`, name locations, the formatter): plain C++ unit tests in the
compiler repo, run by its existing harness. These protect the LSP's data
foundation and catch global-state bleed between analyses.
- Analyze-twice-no-bleed test.
- `resolveModuleFile("net")` → `lib/std/io/net.tr`.
- Formatter golden + idempotence + still-compiles (doc 09), covering every
  `showcase/*.tr`.

### 2. SemanticQuery unit tests (in `turf-lsp`)
The pure functions in `SemanticQuery.cpp` (completion classification, hover text,
token extraction, reference collection) are testable **without** the JSON-RPC
layer: feed a source string → `CompilerService::analyze` → assert on
`getCompletions`/`hoverText`/`getSemanticTokens`/references. Add a GoogleTest/
Catch2 target in `turf-lsp/CMakeLists.txt`. Fast, deterministic, the bulk of
coverage.
- Fixtures: small `.tr` snippets exercising each node kind + each trigger context.
- Table-driven: `(source, cursor) → expected symbol/kind/range`.

### 3. Protocol / integration tests (black-box over stdio)
Drive the actual `turf-lsp` binary with framed JSON-RPC and assert responses.
Best implemented in TypeScript using `vscode-languageclient` test utilities or a
small harness, OR in Python with a minimal LSP client. Cover:
- `initialize` handshake + capabilities.
- didOpen → publishDiagnostics content (codes, ranges, severities).
- completion / hover / definition / references / rename round-trips.
- Cancellation (`$/cancelRequest`), `ContentModified`, ordering under rapid
  didChange (the coalescing/debounce from doc 01).
- Encoding: a fixture with multibyte chars asserts correct ranges under both
  `utf-16` and `utf-8` negotiation.
- Malformed input / unknown method → correct JSON-RPC error, no hang.

### 4. Editor smoke tests
- **VS Code**: `@vscode/test-electron` launches VS Code with the extension,
  opens a `.tr` fixture, asserts diagnostics/hover/completion via the extension
  API. Also verify the VSIX loads in VSCodium (manual or scripted).
- **Neovim**: a headless `nvim --headless` script that attaches the server to a
  `.tr` buffer and asserts `vim.lsp.buf_request` results for hover/definition.

### 5. Performance / soak
- Large-file fixture (generate a big `.tr`): assert analysis + semantic tokens +
  completion stay under a latency budget (e.g. < 200 ms p95 on the worker).
- Soak: replay a recorded editing session (hundreds of didChanges) → assert no
  memory growth (leaked `DocumentAnalysis`/mappers), no queue backlog, no stale
  diagnostics.

## Fixtures
Reuse the real corpus: `turf-lang/showcase/*.tr` and `turf-lang/tests/**` are the
best source of realistic syntax. Add a small `turf-lsp/tests/fixtures/` for
targeted per-feature snippets (broken buffers, multibyte, imports, generics,
enums, methods, extern).

## CI

### Server (`turf-lsp`)
- Build the server (which builds `turfcore` — slow first time due to LLVM 22 +
  the vendored llama.cpp submodule per `AGENTS.md`; cache aggressively).
- Run layers 1–3. Cache the LLVM/CMake build artifacts across runs.
- Platforms: Linux + macOS at minimum (the dev platform is macOS/darwin per this
  environment); Windows is a stretch (LLVM build complexity) — document as
  unsupported initially if it's too costly.

### Extensions
- Lint/build the VS Code extension (`tsc`, `vsce package`).
- Run `@vscode/test-electron` smoke tests.
- Validate the single `turf.tmLanguage.json` parses and tokenizes the showcase.

### Guardrails
- The compiler-side changes (doc 02) must keep `./scripts/test_runner.sh` green —
  run it in CI for any turf-lang change, per `AGENTS.md`. Watch the `_module.tr`
  test-runner traps documented there when adding fixtures.

## Packaging & release

### Server binary
- ✅ Done (2026-09-16): `--version`/`-v` and `--help`/`-h` flags, handled
  before anything else in `main()` (no `Logger::init()`, no stdio-loop
  entry) so they work even if the compiler-facing parts of the binary are
  broken. `serverInfo.version` and the CLI flag now share one source of
  truth -- `include/Version.h.in`, `configure_file`'d from CMake's
  `project(turf-lsp VERSION ...)` into a generated header -- instead of
  the version being a literal `"0.1.0"` hardcoded separately in
  `LSPHandler.cpp` (verified live: a real `initialize` handshake's
  `serverInfo.version` matches `turf-lsp --version`'s output).
- ⬜ Release per-platform `turf-lsp` binaries (GitHub releases); static-link
  LLVM where feasible. Not done -- no CI/release pipeline exists yet in
  this repo to attach binaries to (doc 11's P5.4 marketplace publishing
  has the same blocker).

### Extensions
- VS Code Marketplace **and** Open VSX (forks/VSCodium) — publish the same VSIX
  to both (doc 11).
- The extension resolves the server via `turf.server.path`, PATH, or a bundled
  binary matching the platform; document the precedence.
- Semver the extension independently of the server; record the min compatible
  server version.

### Versioning discipline
- Keep `serverInfo.version`, the extension version, and the advertised LSP
  capabilities in sync; note the LSP spec version targeted (3.17).
- Changelog per release; note which features/methods became available.

## Definition of done (per milestone, from README)
Each milestone (M0–M5) is "done" when: its features pass layers 2–4 for both VS
Code and Neovim, the compiler suite is green, and the capability set advertised in
`initialize` matches what's actually implemented (no advertising a provider that
returns "not implemented").
