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
| Diagnostics | ⚠️ collected, **suppressed** | `publishDiagnostics()` computes `analysis->diagnostics` but sends `json::array()` (empty) "until false-positive sources are sorted." Doc 03 re-enables. |
| Completion | ✅ context-aware | bare / `Ident::` / `instance.`; snippets. Runs a *second* analyze per request; no `import` context; no `resolve`; no scope-precise filtering. |
| Hover | ✅ basic | var/func/method/struct. No doc-comments, no enum/param/field hover. |
| Go to definition | ⚠️ partial + **has a bug** | Local defs work. **Cross-file stdlib path is `lib/std/<name>.tr`, which is wrong now** that modules live in subfolders (`lib/std/io/net.tr`, etc.) — see doc 07. No `declaration`/`typeDefinition`. |
| Semantic tokens | ✅ full | Visitor emits tokens; relies on textual name-loc recovery. No delta/range; no `parameter`/`enum`/`operator` coverage in places. |
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

### Phase 1 — "See errors and colors" (doc 03, 04)
- Re-enable diagnostics with error **codes**, severities (error/warning), and
  `RelatedLocations` → LSP `relatedInformation`. Vet false positives.
- Round out semantic tokens; verify the TextMate grammar as the fast-path
  fallback.

### Phase 2 — "Understand code" (doc 05, 06, 07)
- Completion: add `import` context + scope-precise filtering + `resolve` for
  lazy docs; reuse the cached analysis.
- Hover: doc-comments, enums, params, fields, types.
- Definition/declaration/typeDefinition with correct cross-file resolution.

### Phase 3 — "Navigate" (doc 08)
- Find references + document highlight, workspace-wide (needs the workspace
  index from doc 01).

### Phase 4 — "Change code" (doc 09, 10)
- Formatter (new engine in `turf-lang`, exposed as `turf --format` and reused
  in-process by the LSP).
- Rename (safe, reference-driven) + code actions (quick-fixes from diagnostics,
  "add import", extract, etc.).

### Phase 5 — Polish & ship (doc 11, 12)
- Consolidated VS Code extension published to Marketplace + Open VSX (forks);
  Neovim client + `nvim-lspconfig` upstream entry; test suite + CI.

## Milestone table

| Milestone | Delivers | Depends on |
|---|---|---|
| M0 | Responsive, serialized server; cached analysis; cancellation | ✅ Phase 0 done (2026-09-15) |
| M1 | Live red/yellow squiggles + full highlighting in both editors | M0, Phase 1 |
| M2 | Completion (incl. imports) + hover + go-to-def working end-to-end | M1, Phase 2 |
| M3 | Find references + document highlight | M2, Phase 3 |
| M4 | Format-on-save + rename + quick-fixes | M2, Phase 4 |
| M5 | Published extensions, Neovim client, green CI | all |

## A note on the turf-lang side

Several features are far cleaner (and, for references/rename, only *correct*) if
the compiler exposes a little more. Those changes are collected in doc 02 and
cross-referenced from each feature. They are additive and must not regress the
compiler's own test suite (`./scripts/test_runner.sh` stays green — see
`turf-lang/AGENTS.md`).
