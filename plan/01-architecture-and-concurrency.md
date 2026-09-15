# 01 — Architecture & Concurrency (Foundation)

> Read this before any feature document. Every feature depends on the document
> model, the serialization strategy, and the position-encoding layer defined
> here.

## 1. Process model: in-process compiler reuse

The server links `turfcore` (the compiler as a library, via
`add_subdirectory(../turf-lang)` in `CMakeLists.txt`) and calls the real
pipeline directly:

```
Lexer → Parser (with recovery) → Resolver (2-pass) → TypeChecker
```

`CompilerService::analyze(source, filename)` (see `src/CompilerService.cpp`)
already does this and returns a `DocumentAnalysis { asts, resolver, diagnostics }`.
Codegen is intentionally **not** run — the LSP never needs LLVM IR.

**Keep this design.** It gives exact-semantics parity with `turf` at zero
serialization cost. The entire rest of this document exists to manage the one
liability it creates: global mutable state.

## 2. The global-state problem (the central constraint)

The compiler was written as a batch CLI. Analysis touches process-global mutable
state, including (non-exhaustive):

- Lexer/parser cursor: `CurTok`, `CurLoc`, `g_braceDepth`, `SourceFile`,
  `SourceFilename`, `SourceLines`, `AllSourceLines`, `g_parseRecursionDepth`.
- Parser precedence table: `BinopPrecedence` (must be seeded by
  `InitializePrecedence()` — the constructor already does this).
- `GlobalSymbolTable` (parse-time type registration) via
  `InitializeSymbolTable()`.
- `DiagnosticEngine` — **all static** (`Diagnostics`, `ErrorLines`,
  `CollectAllDiagnostics`, `g_AIEnabled`).
- Error-recovery jump buffer: `g_recoverJmp`, `g_recoverActive`.
- Parsed-AST registry: `ClearParsedTopLevelASTs()`.

`analyze()` already resets these at entry and save/restores the recovery jump
buffer. **Consequence: two analyses cannot run concurrently.** There is exactly
one logical "compiler" in the process.

### Decision: single serialized analysis worker

- One dedicated **worker thread** owns *all* compiler access. Nothing else in
  the process may call `analyze()` or touch compiler globals.
- The main **I/O thread** only reads/frames JSON-RPC and enqueues work; it never
  blocks on analysis.
- A single-consumer work queue sits between them.

```
        stdin  ──►  I/O thread (parse frame, route)
                        │  enqueue(Request)
                        ▼
                  ┌───────────────┐   (owns ALL compiler globals)
                  │ Worker thread │──► CompilerService::analyze(...)
                  └───────────────┘        │
                        │ result           ▼
                        └──────────────►  stdout (framed response)
```

This is the clangd-style model minus the multi-worker pool (we cannot have one,
given the globals). It is enough: analyses are fast (no codegen), and the queue
coalescing below prevents backlog.

### Why not just add mutexes and keep it synchronous?
A mutex would serialize but still **block the read loop** during a slow analyze,
so cancellation and fast notifications (didChange) would queue behind it.
Separating I/O from compute is what makes cancellation and debouncing possible.

### Future option (out of scope, note only)
True parallelism would require threading a per-analysis `CompilerContext` object
through the pipeline to replace the globals. That is a large compiler refactor
(doc 02 §6) and is explicitly **not** required for this plan — one worker is
sufficient for a single-user editor session.

## 3. Request scheduling, debouncing, cancellation

The worker consumes a queue with these rules:

- **Coalesce document updates.** When a newer `didChange` for a URI arrives, drop
  any queued analysis for an older version of that URI. Only the latest version
  is ever analyzed.
- **Debounce didChange** by ~150–250 ms (configurable) so a burst of keystrokes
  triggers one analysis, not one per character.
- **Cancellation.** Implement `$/cancelRequest`. A cancelled request that hasn't
  started is dropped from the queue; one already running checks a per-request
  `std::atomic<bool> cancelled` flag at coarse checkpoints (before resolve,
  before typecheck) and returns a `RequestCancelled` (-32800) error. The
  compiler passes themselves are not interruptible mid-pass — checkpoint
  granularity is acceptable given how fast a pass is.
- **Priority.** Notifications that mutate document state (didOpen/didChange/
  didClose) are processed in order relative to each other; on-demand requests
  (hover/completion/definition) for a URI always run against the latest analyzed
  version of that URI, waiting for a pending analysis if necessary.

### I/O thread hardening (fix existing gaps)
`main.cpp` today: reads a line, if it starts with `Content-Length:` reads the
body. Harden:
- Parse **all** headers until the blank line (don't assume `Content-Length` is
  first or alone); ignore unknown headers; be robust to `\r\n` vs `\n`.
- Read exactly `Content-Length` **bytes** (already done via `cin.read`), but loop
  until fully read (a single `read` may short-read on some platforms).
- Never write logs to stdout (corrupts the stream). Route all logging to stderr
  and/or a file logger (`/tmp/turf-lsp.log` per DEVELOPING.md), gated behind a
  `--log-level` / `TURF_LSP_LOG` env.

## 4. Document model & analysis cache

Replace the ad-hoc `m_documents[uri] = {text, version, analysis}` with a
`DocumentStore`:

```cpp
struct OpenDocument {
    std::string uri;
    std::string text;            // authoritative buffer (post-sync)
    int64_t version;             // client-supplied
    std::string languageId;      // "turf"
    // Cached analysis for THIS version. Shared across all requests.
    std::shared_ptr<const DocumentAnalysis> analysis;
    int64_t analyzedVersion = -1; // version `analysis` corresponds to
    // Precomputed line-start byte offsets for O(log n) position<->offset.
    std::vector<size_t> lineStarts;
};

class DocumentStore {
    std::unordered_map<std::string, OpenDocument> docs; // keyed by uri
    // Mutated only from the worker/dispatch context; reads guarded.
};
```

Key rules:
- **Analyze once per version.** On didOpen/didChange (post-debounce), the worker
  runs `analyze()` once and stores the `shared_ptr<DocumentAnalysis>`. Hover,
  completion, definition, references, semantic tokens, code actions all reuse
  that same cached analysis. **This deletes the current second analyze in
  `handleCompletion`.**
- **Completion's buffer patch** (the `turfLspCursor;` trick in `LSPHandler.cpp`)
  stays, but is applied to a *throwaway* analysis only when the trigger context
  needs a patched parse (doc 05). It must never overwrite the cached
  unpatched analysis.
- `lineStarts` is recomputed on each buffer replacement and powers position
  conversion (§6).

## 5. Incremental sync & incremental analysis

### 5a. Incremental text sync (LSP `textDocumentSync: 2`)
Switch capability from full (`1`) to incremental (`2`). `didChange` then sends
ranged edits (`{range, text}`), not the whole document. Apply each edit to the
buffer using the `lineStarts` index. This reduces bandwidth on large files and
is a prerequisite for accurate change tracking. (Full sync remains a valid
fallback if a client sends it.)

### 5b. Incremental *analysis* (optimization, not required for M0–M2)
The compiler already exposes the machinery:
- AST nodes carry a stable `NodeID` (`include/AST.h`).
- `CollectChangedNodeIDs(oldAST, newAST)` (`include/ASTDiff.h`) diffs two ASTs.
- `LintChangedNodesOnly(E, changedIDs)` re-lints only changed subtrees
  (`include/Lint.h`).

Plan: **do full re-analysis first** (it's cheap without codegen). Only if
profiling on large files shows it matters, add an incremental path that reuses
the prior version's AST for unchanged top-level declarations. Track as a
post-M2 optimization; do not block features on it.

## 6. Position encoding (correctness, do early)

LSP positions are `(line, character)`. **`character` is a UTF-16 code-unit offset
by default.** The compiler works in **1-indexed lines and byte columns**. Three
mismatches to handle everywhere at the boundary:

1. **0-indexed (LSP) vs 1-indexed (compiler).** Already handled per-call
   (`+1`/`-1`) but must be centralized.
2. **UTF-16 vs byte offsets.** A non-ASCII character (e.g. `é`, emoji in a
   string/comment) makes byte column ≠ UTF-16 column. Current code assumes they
   match — wrong for any non-ASCII line.
3. **Negotiate encoding.** In `initialize`, read
   `capabilities.general.positionEncodings`. Advertise support for `utf-16`
   (mandatory) and `utf-8` (preferred — makes conversion a no-op with byte
   columns). Store the negotiated encoding; VS Code today is UTF-16, Neovim
   0.10+ supports `utf-8`.

Introduce a single `PositionMapper` per document (built from `text` +
`lineStarts`) with:
```cpp
size_t toByteOffset(LspPosition p) const;      // (line,char) -> byte offset
LspPosition toLspPosition(size_t byteOffset) const;
SourceLocation toCompilerLoc(LspPosition p) const;   // 1-indexed, byte col
LspRange toLspRange(SourceLocation loc, int byteLen) const;
```
All handlers convert **only** through this mapper. No handler does raw `col-1`
arithmetic anymore.

## 7. Workspace & multi-file model

Turf programs span files via `import`. Features differ in scope:

- **Single-file features** (diagnostics, semantic tokens, hover, completion):
  analyze the open document; imported modules are pulled in transitively by the
  parser's own import resolution (the temp-file trick in `analyze()` keeps
  relative imports working — keep it).
- **Cross-file navigation** (definition into stdlib/user modules): resolve the
  target module's file via the compiler's real module search
  (`IndexModuleDir()`, `TURF_HOME`, compiler-binary-relative fallback — see
  `turf-lang/docs/architecture/modules_and_scope_resolution.md` §2.4). Doc 07.
- **Workspace-wide features** (find references, rename): need an **index of all
  `.tr` files in the workspace roots**. Build a `WorkspaceIndex`:
  - On `initialize`, read `workspaceFolders`.
  - Enumerate `**/*.tr` (respect `.gitignore`-style ignores; skip `build/`,
    `.cache/`, hidden dirs).
  - For each file, lazily parse+resolve on first need and cache a lightweight
    **symbol/occurrence index** (declarations + identifier occurrences with
    locations). Invalidate a file's entry on didChange/save/file-watch events.
  - Register a file watcher (`workspace/didChangeWatchedFiles`) for `**/*.tr`.
  - Budget: index lazily and in the background; never block a request on a full
    workspace scan. Doc 08 details the occurrence index.

`TURF_HOME` handling: the LSP must resolve it identically to the compiler so
stdlib navigation lands on the same files the compiler would compile. Prefer an
explicit `turf.home` client setting → else `TURF_HOME` env → else
compiler-binary-relative fallback (doc 11 wires the setting through).

## 8. Server capabilities (target `initialize` response)

Grow the advertised capabilities incrementally as features land. Final target:

```jsonc
{
  "positionEncoding": "<negotiated>",
  "textDocumentSync": { "openClose": true, "change": 2, "save": { "includeText": false } },
  "diagnosticProvider": { "interFileDependencies": true, "workspaceDiagnostics": false },
  "hoverProvider": true,
  "completionProvider": { "resolveProvider": true, "triggerCharacters": [".", ":"] },
  "definitionProvider": true,
  "declarationProvider": true,
  "typeDefinitionProvider": true,
  "referencesProvider": true,
  "documentHighlightProvider": true,
  "documentSymbolProvider": true,
  "workspaceSymbolProvider": true,
  "semanticTokensProvider": {
    "legend": { "tokenTypes": [...], "tokenModifiers": [...] },
    "range": true, "full": { "delta": true }
  },
  "documentFormattingProvider": true,
  "documentRangeFormattingProvider": true,
  "documentOnTypeFormattingProvider": { "firstTriggerCharacter": "}", "moreTriggerCharacter": [";", "\n"] },
  "renameProvider": { "prepareProvider": true },
  "codeActionProvider": { "codeActionKinds": ["quickfix", "refactor", "source"], "resolveProvider": true },
  "signatureHelpProvider": { "triggerCharacters": ["(", ","] }   // stretch; doc 05
}
```

Note `":"` as a completion trigger is currently a single char; `::` scope
resolution needs the trigger + client re-request behavior handled in doc 05.

## 9. Error handling & protocol conformance

- Every request gets exactly one response (result or error) with the matching
  `id`. Add a small typed dispatcher that guarantees this even on exceptions
  (today a thrown `json::exception` is swallowed with no response — the client
  hangs waiting).
- Standard JSON-RPC error codes: `-32601` MethodNotFound, `-32602`
  InvalidParams, `-32603` InternalError, `-32800` RequestCancelled,
  `-32801` ContentModified (return this when a request's document version was
  superseded before it ran).
- Respect `initialize` ordering: reject requests before `initialize`; ignore
  most after `shutdown` except `exit`.

## Phased tasks (this document)

All of P0.1–P0.6 are done (2026-09-15). Summary of what actually landed,
including where it diverges from what's written above:

- **P0.1 ✅.** `LSPHandler` now runs two background threads besides the I/O
  thread that calls `handleMessage()`: a worker thread (`workerLoop()`) that
  is the ONLY code in the process allowed to touch compiler globals —
  `CompilerService::analyze()`, `SemanticQuery::*`, `ResolveModuleFile()` all
  run there and nowhere else — fed by a `std::deque<std::function<void()>>`
  work queue; and a debounce thread (`debounceLoop()`) that coalesces rapid
  `didChange` bursts (a new edit for a uri just overwrites its pending due
  time) into one `publishDiagnostics`-triggering task per ~200ms quiet
  period (`TURF_LSP_DEBOUNCE_MS` to override). **Divergence from the
  original design sketch:** on-demand requests (hover/definition/
  completion/semanticTokens) do NOT wait on or coordinate with the debounce
  timer at all — each one calls `ensureFreshAnalysis()` the instant it runs
  on the worker thread, which re-analyzes inline if the cached analysis's
  `analyzedVersion` is behind the document's current text version. This
  sidesteps the "wait for a pending analysis" coordination problem entirely
  (no request can ever see stale text) at the cost of a request landing
  mid-burst paying for its own extra analyze() call — acceptable since
  analysis is cheap (no codegen) and typing-speed bursts are short.
- **P0.2 ✅.** Real `DocumentStore` class (`include/DocumentStore.hpp`,
  `src/DocumentStore.cpp`) replaced the ad-hoc `m_documents`/`DocumentState`
  map entirely: `shared_ptr<const DocumentAnalysis>` + `analyzedVersion`,
  internally mutex-guarded (short critical sections only — analysis never
  runs while the lock is held, so the I/O thread's text updates are never
  blocked behind it). `setAnalysis()` is monotonic (won't let a stale
  analysis clobber a newer one). The completion `Ident::` fast-path from the
  previous session now goes through `ensureFreshAnalysis()` like every
  other on-demand request, rather than reading a possibly-stale raw pointer
  directly — necessary now that analysis is asynchronous (debounced)
  instead of happening synchronously inline on every didChange.
- **P0.3 ✅.** `PositionMapper` (`include/PositionMapper.hpp`,
  `src/PositionMapper.cpp`) does real UTF-8-aware byte↔UTF-16/UTF-8/UTF-32
  conversion (decodes codepoints, counts surrogate pairs for astral
  characters). `positionEncoding` is negotiated in `initialize` from
  `capabilities.general.positionEncodings` (prefers `utf-8` if offered).
  Routed through **every** handler that crosses the LSP/compiler boundary,
  including completion's trigger-text slicing (`lineTextUpToByteOffset`/
  `patchForCompletionAt`, which used to treat the LSP `character` as a raw
  byte index — a real latent bug for any non-ASCII content before the
  cursor, fixed as part of this pass, not left for later). One known,
  deliberate gap: go-to-definition's range for a **cross-file** target
  (e.g. into a stdlib module) still uses raw 1-indexed→0-indexed arithmetic
  instead of a real mapper, because the target file's text isn't loaded in
  this document's `DocumentStore` to build one from — correct as long as
  the target identifier is ASCII, true for every turf-lang identifier.
- **P0.4 ✅.** `LSPFramer` (`include/LSPFramer.hpp`, `src/LSPFramer.cpp`)
  parses all headers until the blank line, tolerates bare `\n`, loops to
  read exactly `Content-Length` bytes. `Logger` (`include/Logger.hpp`,
  `src/Logger.cpp`) routes everything to stderr and, if `TURF_LSP_LOG`
  names a path, also to that file. `$/cancelRequest` sets a per-request
  `shared_ptr<atomic<bool>>` flag checked at entry to each `do*` handler and
  at `CompilerService::analyze()`'s two natural phase boundaries (after
  parse, before resolve; after resolve, before typecheck — coarse
  checkpoints, not mid-pass interruption, matching what this doc originally
  scoped). `dispatch()` wraps every message in a try/catch that guarantees
  exactly one response even on an unexpected exception (`-32603`); unknown
  methods get `-32601`; requests before `initialize`/after `shutdown` get
  `-32002`/`-32600`; a request whose document closed out from under it gets
  `-32801` (`ContentModified`).
- **P0.5 ✅.** `textDocumentSync.change: 2`. `DocumentStore::applyRangedEdit()`
  builds a `PositionMapper` over the CURRENT text, converts the edit
  range's start/end to byte offsets, splices. A `contentChanges` entry
  without a `range` key still does a full replace (spec-legal fallback,
  handled per-event so a client can even mix the two forms in one batch).
- **P0.6 ✅ (skeleton, as scoped).** `WorkspaceIndex`
  (`include/WorkspaceIndex.hpp`, `src/WorkspaceIndex.cpp`) walks
  `workspaceFolders`/`rootUri` synchronously at `initialize` (cheap — stats
  paths only, never opens file content), skipping `build`/`turflang_build`/
  dot-directories, and stores the `.tr` file list. Deliberately does NOT
  yet: parse/index symbols (doc 08), register a
  `workspace/didChangeWatchedFiles` dynamic-registration round-trip with
  the client, or do incremental add/remove on file create/delete — all
  explicitly out of scope for this skeleton per the original text below.

Not part of doc 01 and NOT done here: doc 02 item A's `turf::query::analyze`
facade (the turf-lang-side wrapper around the reset/save/restore dance) —
see `02-compiler-core-api-changes.md`, which itself downgraded this from
"hard blocker" to "nice-to-have" once `ResolveModuleFile()` shipped.
`CompilerService::analyze()` still open-codes the dance; it gained one new
piece of state to restore (the `isCancelled` checkpoint plumbing) but the
overall shape is unchanged.

Original task list, kept for reference:

- **P0.1** Split I/O thread from a single analysis worker thread; add the work
  queue with coalescing + debounce.
- **P0.2** `DocumentStore` + one cached `DocumentAnalysis` per version.
- **P0.3** `PositionMapper` + encoding negotiation; route all handlers through
  it.
- **P0.4** `$/cancelRequest`, `ContentModified`, guaranteed one-response
  dispatch, header-parsing hardening, stderr/file logging.
- **P0.5** Switch to incremental text sync (`change: 2`) with full-sync
  fallback.
- **P0.6** (deferred) `WorkspaceIndex` skeleton + file watcher — needed by doc
  08; land the interface now, fill in with references.

## Acceptance tests (foundation)

- Rapid typing (simulate 20 didChanges in 300 ms) yields **one** analysis and no
  stale diagnostics for superseded versions.
- A hover request during an in-flight analysis returns against the latest
  completed version and never deadlocks.
- A file containing a multibyte character before an identifier returns correct
  hover/definition ranges (proves position mapping).
- `$/cancelRequest` for a queued completion prevents its analysis from running.
- Malformed/oversized headers don't wedge the read loop; unknown methods return
  `-32601`, not silence.
