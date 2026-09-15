# Turf LSP — Implementation Plan

This directory is the authoritative, feature-by-feature implementation plan for
bringing `turf-lsp` from its current partial state to a complete language server
that works, at minimum, in **VS Code (and its forks — Cursor, Windsurf, VSCodium)**
and **Neovim**, and by extension any LSP 3.17 client.

It is a *plan only*. No feature here is implemented yet beyond what the
"Current state audit" section marks as ✅.

## How to read this

Start here, then read the two foundation documents before any feature document,
because every feature depends on them:

| # | Document | What it covers |
|---|----------|----------------|
| — | `README.md` (this file) | Scope, principles, current-state audit, phased roadmap, milestones |
| 01 | `01-architecture-and-concurrency.md` | In-process core reuse, the global-state problem, threading/serialization, document manager, incremental analysis, position encoding, workspace indexing |
| 02 | `02-compiler-core-api-changes.md` | Changes required **inside `turf-lang`** to support the LSP cleanly (a stable query facade, name-token locations, reentrancy, a references index, a formatter engine) |
| 03 | `03-diagnostics.md` | `publishDiagnostics` — errors + warnings (squiggly lines) |
| 04 | `04-code-highlighting.md` | Semantic tokens + the TextMate grammar (syntax highlighting) |
| 05 | `05-completion.md` | Auto-complete: identifiers, functions, module imports, members |
| 06 | `06-hover.md` | `textDocument/hover` |
| 07 | `07-goto-definition-declaration.md` | `definition` + `declaration` + `typeDefinition` |
| 08 | `08-find-references.md` | `references` + `documentHighlight` |
| 09 | `09-formatting.md` | `formatting` + `rangeFormatting` + `onTypeFormatting` |
| 10 | `10-refactoring-and-rename.md` | `rename`/`prepareRename` + `codeAction` (quick fixes + refactors) |
| 11 | `11-editor-clients.md` | VS Code extension (+ forks) and Neovim client, packaging |
| 12 | `12-testing-ci-and-packaging.md` | Test harness, fixtures, CI, release/versioning |

Each feature document (03–10) follows the same skeleton:
**Goal → LSP method(s) & capability → Current state → Compiler data consumed →
Design/algorithm → Edge cases → Phased tasks → Acceptance tests.**

## Scope (the required feature set)

The user-facing minimum, mapped to LSP methods:

| Requested feature | LSP method(s) | Plan doc |
|---|---|---|
| Auto-complete suggestions | `textDocument/completion`, `completionItem/resolve` | 05 |
| Module import suggestions | `textDocument/completion` (import context) | 05 |
| Function suggestions | `textDocument/completion` (call context) | 05 |
| Code highlighting | `textDocument/semanticTokens/*` + TextMate grammar | 04 |
| Squiggly lines | `textDocument/publishDiagnostics` | 03 |
| Go to definition / declaration | `textDocument/definition`, `.../declaration`, `.../typeDefinition` | 07 |
| Diagnostics (errors + warnings) | `textDocument/publishDiagnostics` | 03 |
| Hover information | `textDocument/hover` | 06 |
| Find references | `textDocument/references`, `.../documentHighlight` | 08 |
| Code formatting | `textDocument/formatting`, `.../rangeFormatting`, `.../onTypeFormatting` | 09 |
| Refactoring | `textDocument/rename`, `.../prepareRename`, `.../codeAction` | 10 |

Editor targets: **VS Code + forks and Neovim (0.10+, built-in `vim.lsp`)**. Emacs
(`eglot`/`lsp-mode`) and Sublime (`LSP`) come free from spec compliance and are
smoke-tested only (doc 11).

## Guiding principles

1. **Reuse the compiler in-process; never shell out.** The server already links
   `turfcore` and calls `Lexer → Parser → Resolver → TypeChecker` directly. This
   is the right call (no serialization/FFI overhead, exact-same semantics as the
   real compiler) and the plan keeps it. The cost is the compiler's global
   mutable state, which forces strict request serialization — see doc 01.

2. **The compiler is the single source of truth.** Every semantic answer
   (types, symbols, diagnostics, definition targets) comes from the real
   Resolver/TypeChecker, never from a parallel re-implementation in the LSP.
   Only *textual* fallbacks (e.g. recovering a name-token column that the AST
   doesn't store) are permitted, and doc 02 aims to delete even those by fixing
   the parser to record name locations.

3. **Degrade gracefully on broken buffers.** The buffer is syntactically invalid
   on nearly every keystroke. The recovery parser + per-statement `setjmp`
   resync in `CompilerService::analyze()` already delivers a partial AST; every
   feature must produce useful results from a *partial* analysis and never crash
   or hang on invalid input.

4. **One analysis per document version, cached and shared.** Today completion
   runs a *second* full `analyze()` per keystroke on top of the didChange
   analysis. Doc 01 introduces a single cached `DocumentAnalysis` per `(uri,
   version)` that every request reuses.

5. **Ship a thin vertical slice per phase.** Each phase below is independently
   demoable in both VS Code and Neovim.

## Current state audit

Read the existing code before implementing: `src/CompilerService.cpp`,
`src/SemanticQuery.cpp`, `src/LSPHandler.cpp`, `src/main.cpp`.

| Capability | State | Notes |
|---|---|---|
| JSON-RPC framing over stdio | ✅ done (2026-09-15) | `LSPFramer::readMessage()` parses all headers until the blank line, tolerates bare `\n`, loops to read exactly `Content-Length` bytes. `$/cancelRequest` implemented; every dispatch path guarantees exactly one response (id-keyed try/catch → `-32603` on an unexpected exception); `-32601`/`-32002`/`-32600`/`-32800`/`-32801` all wired. |
| Lifecycle (initialize/initialized/shutdown/exit) | ✅ works | `initialize` advertises hover/definition/semanticTokens/completion, negotiated `positionEncoding`, incremental `textDocumentSync`. |
| Document sync | ✅ incremental (2026-09-15) | `textDocumentSync.change: 2`. `DocumentStore::applyRangedEdit()` splices ranged edits through `PositionMapper`; a range-less change event still falls back to a full replace (spec-legal, and what `change: 1` clients send). |
| Diagnostics | ✅ done (2026-09-15) | Real publish: `code`, `source`, `relatedInformation`, hint-in-message, token-widened ranges (buffer-scan fallback — doc 02 §E's real end-columns never landed), multi-URI publish for errors attributed to an imported file, clear-on-clean, version tagging. `setCollectAllDiagnostics(true)`, `g_AIEnabled` forced off. Found/fixed along the way: typed errors baked raw ANSI color escapes into the message text — stripped before reaching the client. Doc 03's P1.5 (field-testing) and P1.6 (pull model) still open. |
| Completion | ✅ context-aware | bare / `Ident::` / `instance.`; snippets. Runs a *second* analyze per request; no `import` context; no `resolve`; no scope-precise filtering. |
| Hover | ✅ basic | var/func/method/struct. No doc-comments, no enum/param/field hover. |
| Go to definition | ✅ local + cross-file | Cross-file resolution goes through `ResolveModuleFile()` (the real module search, same as the compiler's own `import` resolution) — the old hardcoded `lib/std/<name>.tr` bug is fixed. No `declaration`/`typeDefinition`. |
| Semantic tokens | ✅ full + delta | Visitor emits tokens; relies on textual name-loc recovery. `full/delta` implemented (int-level diff + `resultId` cache, doc 04 P1.d). `parameter`/type-annotation coverage done (P1.c, 2026-09-15) — params (decl + every use), var-decl `: T`, and struct-field name+`: T` now tokenize, using new turf-lang-side spans. No `range` tokens (P2.e). |
| Find references | ❌ none | Doc 08. |
| Formatting | ❌ none | No formatter exists anywhere in the codebase. Doc 09 builds one. |
| Rename / code actions | ❌ none | Doc 10. |
| Threading | ✅ done (2026-09-15) | I/O thread only frames/parses + does cheap bookkeeping; one worker thread owns all compiler access (`CompilerService::analyze()`, `SemanticQuery::*`, `ResolveModuleFile()` — all touch process-global mutable state per doc 01 §2). A debounce thread coalesces rapid `didChange` bursts into one `publishDiagnostics` per quiet period; on-demand requests don't wait on it — `ensureFreshAnalysis()` self-heals to the latest text version the instant a request actually runs. Doc 01. |
| Editor clients | ⚠️ scaffolded | Two parallel VS Code extension trees exist (`editors/vscode/` and `vscode-turf/`) — **consolidate** (doc 11). Neovim = `lspconfig` snippet only; needs a native `vim.lsp.config` path too. |

### Two things to fix up front (cheap, high-value)
- **The stdlib definition path bug** (doc 07): resolve module files through the
  same `IndexModuleDir()`/`TURF_HOME` logic the compiler uses, not a hardcoded
  `lib/std/<name>.tr`.
- **Duplicate VS Code extension directories**: pick one (`editors/vscode/`),
  delete/merge the other (`vscode-turf/`) to stop drift.

## Phased roadmap

Each phase is a shippable increment. Phase 0 is foundational and unblocks
everything; do it first.

### Phase 0 — Foundation (doc 01, 02) — ✅ doc 01's P0.1–P0.6 DONE (2026-09-15)
- ✅ Serialize all requests onto a single worker thread; keep the read loop
  responsive; add `$/cancelRequest` handling. (P0.1, P0.4)
- ✅ Introduce a `DocumentStore` keyed by uri with **one** cached
  `DocumentAnalysis` (`analyzedVersion`-tracked, `shared_ptr`-shared with
  readers); the redundant second analyze in completion stays deleted (only
  the throwaway *patched* analysis for a dangling `.`/`::` trigger still
  runs a second time, by design — see doc 05 G1). (P0.2)
- ✅ Position-encoding negotiation (UTF-16 default; UTF-8 if the client
  supports it) and a byte↔UTF-16 `PositionMapper`, threaded through every
  handler (hover, definition, semanticTokens, completion's trigger-text
  slicing) — not just the ones doc 01 called out by name. (P0.3)
- ✅ Incremental text sync (`textDocumentSync.change: 2`), header-parsing
  hardening, stderr/file logging (`TURF_LSP_LOG`). (P0.5, rest of P0.4)
- ✅ `WorkspaceIndex` skeleton: enumerates `**/*.tr` under the workspace
  root(s) at `initialize`, skips `build`/hidden dirs; doc 08 fills in the
  actual symbol/occurrence index and file-watcher-driven incremental
  updates later. (P0.6, deliberately left as the inventory-only skeleton
  the doc scoped it as)
- ⬜ Not done, and correctly so per doc 02's own note: the `turf::query::analyze`
  facade (doc 02 item A's larger half) remains a turf-lang-side refactor doc
  02 itself downgraded from "hard blocker" to "nice-to-have" once
  `ResolveModuleFile()` shipped — `CompilerService::analyze()` still
  open-codes the reset/save/restore dance. Worth doing eventually (keeps the
  global-state contract in one place, tested by turf-lang's own suite) but
  isn't required for anything doc 01 needed; a good candidate for its own
  future session rather than folded into this one.

### Phase 1 — "See errors and colors" (doc 03, 04) — ✅ core done (2026-09-15)
- ✅ Diagnostics re-enabled with error **codes**, severities (error/warning),
  and `RelatedLocations` → LSP `relatedInformation`, token-widened ranges,
  multi-URI publish, clear-on-clean, version tagging (doc 03 P1.1–P1.4). Not
  done: P1.5 (field-testing against real editor usage — no live editor session
  available this session) and P1.6 (optional pull model, explicitly "later").
- ✅ `semanticTokens/full/delta` implemented (doc 04 P1.d); P1.b (encoding
  correctness) verified as already structurally correct, no change needed.
  ✅ P1.c done (2026-09-15): the compiler-side gap (`TurfType` had no source
  span, `Symbol` had no parameter-vs-local flag) is now closed in turf-lang
  (`VariableSymbol::IsParameter`, `ParamDecl`/`VarDeclExprAST`/`StructField`
  gaining `TypeLoc`/`NameLoc` spans) and consumed on the LSP side —
  parameters, var-decl type annotations, and struct fields now tokenize.
  Not done: P1.a (grammar-directory consolidation — real blast-radius
  file/dir work that doc 11 already owns, see "two things to fix up front"
  above).

### Phase 2 — "Understand code" (doc 05, 06, 07) — ✅ done (2026-09-16)
- ✅ Completion: `import <prefix>` module-name context and
  `import mod::<prefix>` symbol listing (the latter via a fully separate
  analysis of the target module's own file -- it isn't imported into the
  current one yet) both shipped (doc 05 G2). `completionItem/resolve`
  implemented (lazy doc-comment fill, G4). Tiered `sortText` +
  scope-distance ranking for locals (G3/G6). Not done: chained member
  access/`self.` edge cases (P2.6), `SuggestionIndex` trie pre-filtering
  (P2.5), signature help (P3.7, explicitly a stretch goal).
- ✅ Hover: `range` on every result; struct field access, enum members,
  module-import names, doc-comments (interim LSP source re-scan), and
  overload listing at a bare function name all working (doc 06
  P2.1–P2.4, fully done). A long-standing separate bug found and fixed
  along the way: hovering/going-to-definition on a bare (unqualified)
  free-function call site returned nothing at all -- only
  module/struct-qualified calls worked before this.
- ✅ Definition (cross-file fix, prior session) + declaration + typeDefinition
  (including cross-file into an imported module) + `LocationLink` (with
  `originSelectionRange` and a real target-name-width
  `targetSelectionRange`, not a 1-char caret) all working (doc 07,
  fully done).
- Found and fixed along the way (not originally scoped): `CompilerService::
  analyze()` never called `ParsePreludeImports()`, so the LSP silently
  never auto-imported `int`/`double`/`bool`/`char`/`strings`/`array` into
  any file it analyzed — ordinary code relying on a prelude-provided
  operator/UFCS method with no explicit import falsely failed to
  typecheck only under the LSP, never under the real `turf` CLI. Fixed;
  every visitor that walks `asts` for user-facing output now skips the
  synthesized prelude entries via `DocumentAnalysis::preludeImportCount`.
- Known limitation, not fixed this session: no AST visitor handles
  `RecoverExprAST` (`recover ... produce ...` / `recover as err run
  {...}`), so hover/definition/typeDefinition/semantic-tokens/completion
  are all blind to anything textually inside one. See doc 06's Edge
  cases section for detail.

### Phase 3 — "Navigate" (doc 08) — ✅ core done (2026-09-16)
- ✅ `documentHighlight` (in-file) + `references` (workspace-wide,
  `includeDeclaration` honored) + `documentSymbol` (flat outline), all
  shipped on a name+scope identity heuristic rather than waiting on
  compiler item C's `symbolId()` (doc 02 §C) -- verified across a real
  3-file workspace and a shadowing case (two same-named locals in
  different functions correctly kept separate). Not done: Read/Write
  tagging on highlights, struct-field/enum/module-name occurrences,
  persistent/incremental indexing (every `references` request re-scans
  the workspace fresh instead), `workspace/symbol`, `$/progress`.

### Phase 4 — "Change code" (doc 09, 10) — ✅ core done (2026-09-16)
- ✅ Formatter: `turf::format::format()` (indentation-only, see doc 09) +
  `turf --format[ --write]` + `textDocument/formatting`, one engine,
  two callers. Not done: `rangeFormatting`, `onTypeFormatting`, the
  fuller operator/spacing rule set doc 09 originally specced.
- ✅ Rename: `prepareRename` + `rename`, reusing doc 08's workspace
  occurrence scan directly (rename IS find-references + text
  replacement). Verified workspace-wide across a real 3-file project,
  keyword rejection, and the stdlib guard (refuses renaming a real
  stdlib function). Not done: the "would newName collide with an
  existing visible declaration" conflict check.
- ✅/⬜ Code actions: 2 quick-fixes shipped (2026-09-16) -- "add import"
  (E032, verified end-to-end: applying it makes a real broken file
  compile) and "prefix unused var with underscore" (logic verified via
  a synthetic diagnostic -- see the found-but-unfixed gap below for
  why). Not done: the full `E0NN`-to-quick-fix table, did-you-mean,
  extract-variable, organize-imports.
- **Found, not fixed** (belongs on doc 03, discovered while testing code
  actions): `CompilerService::analyze()` never runs the CFG-based lint
  pass at all (unused-variable/dead-store warnings live in Codegen, a
  phase the LSP intentionally skips) -- so that whole class of warning
  never reaches `publishDiagnostics` today, confirmed live against the
  real `turf` CLI on identical source.

### Phase 5 — Polish & ship (doc 11, 12)
- Consolidated VS Code extension published to Marketplace + Open VSX (forks);
  Neovim client + `nvim-lspconfig` upstream entry; test suite + CI.

## Milestone table

| Milestone | Delivers | Depends on |
|---|---|---|
| M0 | Responsive, serialized server; cached analysis; cancellation | ✅ Phase 0 done (2026-09-15) |
| M1 | Live red/yellow squiggles + full highlighting in both editors | ✅ server-side core done (2026-09-15); editor-side verification (P1.5) and the grammar-directory consolidation (P1.a, doc 11) still open |
| M2 | Completion (incl. imports) + hover + go-to-def working end-to-end | ✅ done (2026-09-16) |
| M3 | Find references + document highlight | ✅ core done (2026-09-16) — incremental indexing/`workspace/symbol` still open |
| M4 | Format-on-save + rename + quick-fixes | ✅ format + rename + 2 quick-fixes done (2026-09-16) — full quick-fix table/refactors not started |
| M5 | Published extensions, Neovim client, green CI | all |

## A note on the turf-lang side

Several features are far cleaner (and, for references/rename, only *correct*) if
the compiler exposes a little more. Those changes are collected in doc 02 and
cross-referenced from each feature. They are additive and must not regress the
compiler's own test suite (`./scripts/test_runner.sh` stays green — see
`turf-lang/AGENTS.md`).
