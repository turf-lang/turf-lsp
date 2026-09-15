# 03 — Diagnostics (errors + warnings / squiggly lines)

## Goal
Show live red squiggles for errors and yellow for warnings as the user types,
each with the compiler's stable error code, hint, and related locations.

## LSP surface
- Push model: `textDocument/publishDiagnostics` notification (primary).
- Pull model (optional, later): `textDocument/diagnostic` +
  `diagnosticProvider` capability. VS Code and Neovim both support push; start
  with push. Add pull only if we want on-demand refresh semantics.

## Current state
`CompilerService::analyze()` already builds `analysis->diagnostics` as LSP JSON,
but `LSPHandler::publishDiagnostics()` **sends an empty array** deliberately
("until the false-positive sources this was turned off for are fully sorted").
Also, the mapped range is only 1 char wide, uses `severity` correctly
(1=error/2=warning), but drops `code`, `relatedInformation`, and `hint`.

## Compiler data consumed
`DiagnosticEngine::Diag` (see `include/Lexer.h`) already carries everything:
`Loc` (line/col/filename), `Msg`, `Hint`, `IsWarning`, `RelatedLocations`
(structural context), `Category` (SLM enum), `Filename`, `Code` (`E0NN`).
Warnings also flow through the same engine (`TurfError::warn()` → `add(...,
IsWarning=true)`); lint warnings come via `Lint.cpp`.

Two flags matter:
- `DiagnosticEngine::setCollectAllDiagnostics(true)` — its own comment names the
  LSP: without it, `add()` keeps only **one** primary error per source line, so a
  second genuine error on the same physical line is dropped. The LSP must set it
  for analysis passes.
- `g_AIEnabled` — leave **off**. `--smart` SLM suggestions are slow/nondeterministic
  and inappropriate for per-keystroke analysis. (SLM text can optionally be
  fetched lazily on a code-action, doc 10, never inline.)

## Design

### Mapping `Diag` → LSP `Diagnostic`
```jsonc
{
  "range": <PositionMapper.toLspRange(Diag.Loc, tokenLen)>,
  "severity": Diag.IsWarning ? 2 : 1,
  "code": Diag.Code,                        // "E017"
  "codeDescription": { "href": "<docs URL or file>/errors/codes/E017.txt" },
  "source": "turf",
  "message": Diag.Msg + (Diag.Hint.empty()? "" : "\nhint: " + Diag.Hint),
  "relatedInformation": [ for each RelatedLocation:
     { "location": {uri, range}, "message": rel.note } ]
}
```
- **Range width**: use the real end column from compiler item E (doc 02 §E) when
  present; otherwise widen from `Loc` to the end of the identifier/token under it
  using the buffer, falling back to 1 char. A whole-token squiggle reads far
  better than a caret.
- **relatedInformation**: `RelatedLocations` may point into *other* files
  (imports). Convert each to a proper `file://` URI via the module resolver
  (doc 02 §A). This gives "second definition was here" cross-file squiggle links.
- **Multi-file**: an error surfaced in an imported module while analyzing the
  open file has `Diag.Filename` ≠ the open doc. Publish those against the
  imported file's URI too (or at least attach as relatedInformation), so the user
  isn't told "there's an error" with no navigable location. Keep a set of URIs we
  published non-empty diagnostics to, and clear them when they go clean.

### Publishing lifecycle
- On didOpen / debounced didChange: worker runs the cached analysis (doc 01),
  converts diagnostics, publishes for the doc URI **and** any secondary URIs that
  received diagnostics this run; explicitly publish an **empty** array for any URI
  that previously had diagnostics but is now clean (otherwise stale squiggles
  linger).
- On didClose: publish empty diagnostics for that URI (already done today) and
  drop it from the "dirty URIs" set.
- Attach `version` to `publishDiagnostics` params so the client can discard
  diagnostics for superseded versions.

### Coded vs. codeless diagnostics (syntax errors have no `E0NN`)
The compiler has two raise paths and they differ in what they carry:
- **Typed** (`SomeError(...).raise()`) → carries `Code` (`E0NN`), `Hint`,
  `Category`, `RelatedLocations`.
- **Raw** (`LogErrorAt(Loc, Msg)` → `DiagnosticEngine::add(Loc, Msg,
  isMessageWarning(Msg))`) → carries **location + message only**; `Code` is `""`,
  `Category` defaults `GENERAL`, no hint/related, and **severity is inferred from
  the message text** (`isMessageWarning`).

The parser uses the **raw path for the large majority of syntax errors** (~144
`LogErrorAt` sites across `Parser*.cpp` vs ~24 typed `raise()`); the `E001–E068`
registry is overwhelmingly *semantic/runtime* (array, memory, control-flow,
declaration, type, module). Only `E014 KeywordError` / `E015 SyntaxError` are
typed syntax errors.

**This is fine for diagnostics.** `code` is optional in the LSP `Diagnostic`
type — a codeless diagnostic still renders a full squiggle + message + severity,
and every `LogErrorAt` still supplies a real `SourceLocation`. So **all syntax
errors surface as squiggles today**, coded or not. The mapping above simply omits
`code`/`codeDescription` when `Diag.Code` is empty.

**What the missing codes cost, and the fallback:**
- No `--explain` href for syntax errors (negligible).
- No stable **join key for code-action quick-fixes** (doc 10 keys fixes on
  `code`). For codeless syntax diagnostics, doc 10's quick-fix matcher must fall
  back to **message-pattern matching** (e.g. `message.startsWith("Expected ';'")
  → "Insert ';'"`), which is brittle and must be kept in sync with the compiler's
  wording.
- No stable identity for tests/telemetry.

**Optional compiler-side fix (recommended, not a blocker — see doc 02 §E2):**
give the common parser/lexer `LogErrorAt` sites stable codes, either by routing
them through typed `TurfError` subclasses or by adding an overload
`LogErrorAt(Loc, Msg, Code)` that threads a code into `DiagnosticEngine::add`.
That upgrades syntax-error UX (explain links, reliable quick-fixes, message-
independent tests) without changing anything on the LSP side — the mapping
already forwards whatever `Code` is present.

### The false-positive problem (why it was disabled)
Diagnostics were suppressed because mid-edit buffers produce spurious errors.
Mitigations, in order:
1. **Analyze the latest debounced version only** (doc 01) — avoids publishing
   errors from a half-typed intermediate state.
2. **Recovery parser already localizes** damage to the statement under edit
   (per-statement `setjmp` resync). Errors after the cursor's broken statement
   are still meaningful.
3. **Confidence tiers.** Classify diagnostics:
   - *Parse/lex errors* on the line currently being edited are often transient →
     optionally debounce these an extra beat, or mark them but let them settle.
   - *Resolver/TypeChecker errors* (undeclared name, type mismatch) are usually
     real → publish immediately.
   A simple first cut: publish everything from the settled analysis; if field
   testing shows noise, gate *parser-syntax* diagnostics on "the edited line is
   not the line the user's cursor is on." Do **not** re-suppress everything.
4. **Never invent diagnostics in the LSP.** Only forward what the compiler
   raised.

### Diagnostic quick-fixes
Each diagnostic with a known `Code` can carry an associated code action (doc 10)
— e.g. E-code for "unknown module" → "add `import X;`". The diagnostic's `code`
field is the join key. Plan the mapping table in doc 10; diagnostics just need to
emit the `code` faithfully (done via mapping above).

## Edge cases
- Diagnostic at EOF / empty buffer → clamp range into valid bounds via
  `PositionMapper`.
- Column 0 / line 0 sentinels (`{0,0}` hand-built locations) → skip or clamp;
  never emit a negative LSP position.
- Non-ASCII lines → ranges must go through `PositionMapper` (byte→UTF-16).
- A file that fails to even tokenize → still return the lex diagnostics that were
  produced; don't drop the whole batch.
- Warnings on a line that also has an error: the engine drops the warning by
  design (`add()` doc). With `setCollectAllDiagnostics(true)` errors are kept;
  confirm warning behavior and decide if the LSP wants both (probably keep the
  engine's default — error dominates the line).

## Phased tasks
- ✅ **P1.1** `setCollectAllDiagnostics(true)`/restored around each `analyze()`
  call (`CompilerService.cpp`); `g_AIEnabled` forced `false` unconditionally at
  the top of `analyze()` rather than trusted to already be that way.
- ✅ **P1.2** Re-enabled publishing. `CompilerService::analyze()` now builds a
  compiler-native intermediate (`MappedDiagnostic`/`MappedDiagLocation`, see
  `DocumentAnalysis.hpp`) instead of pre-baked LSP JSON, so `LSPHandler` can do
  encoding-aware range conversion; `doPublishDiagnostics()` maps that to real
  `Diagnostic` objects with `code`, `source: "turf"`, `relatedInformation`, and
  hint folded into `message` (LSP has no separate hint field). `codeDescription`
  deliberately **not** emitted — there's no hosted docs URL for `E0NN` codes
  (only local `docs/errors/codes/*.txt` files in the turf-lang repo, reachable
  today only via `turf --explain`), and a `file://` link to an arbitrary local
  install path isn't something the LSP can construct without knowing the
  compiler's own install root, which isn't plumbed to it. Not a blocker —
  `codeDescription` is optional in the spec.
- ✅ **P1.3** Real end-columns from doc 02 §E never landed (still not part of
  this session's scope), so this is the buffer-widen fallback as planned:
  `widenDiagTokenByteLen()` scans `AllSourceLines` for the identifier/token run
  starting at the diagnostic's point location, falling back to 1 byte for
  anything non-identifier-shaped (punctuation, a hand-built `{0,0}` sentinel).
- ✅ **P1.4** Multi-URI publish + clear-on-clean + version tagging, all in
  `doPublishDiagnostics()`: diagnostics are grouped by the real file each one is
  attributed to (`MappedDiagLocation::filename`, which can differ from the
  analyzed document for an error surfaced while resolving an `import`),
  published per-uri, and any uri that had non-empty diagnostics on the
  *previous* publish for this primary document but not this one gets an
  empty-array publish to clear its now-stale squiggle (`m_secondaryDiagUris`).
  `handleDidClose` does the same on close. The primary document's own publish
  carries `version` (from the analyzed snapshot); secondary/imported-file
  publishes don't (no independently-tracked version for a file that may not
  even be open) — legal per spec (`version` is optional).
  Cross-file ranges use the same raw 1-indexed→0-indexed arithmetic
  `doDefinition()`'s cross-file branch already established, rather than a real
  `PositionMapper` (no buffer loaded for a file that isn't open) — correct as
  long as the span is ASCII, true for every identifier turf-lang's lexer
  accepts.
- ⬜ **P1.5** Not field-tested against real editor usage this session (no live
  editor session available); the false-positive concern this task exists for
  didn't reproduce in testing — `ensureFreshAnalysis()` (doc 01) already
  analyzes only the settled, debounced text, and the two-errors-one-line
  acceptance test below confirms `setCollectAllDiagnostics` is doing its job.
  Revisit once there's real dogfooding signal; don't preemptively add
  syntax-error gating without evidence it's needed.
- ⬜ **P1.6** Deliberately not done — explicitly scoped as "(later)" / optional
  in this same doc.

**Bug found and fixed along the way (not in the original task list):** several
typed compiler errors (e.g. `NameError` in `ErrorsScope.h`) bake raw ANSI color
escape codes directly into `Diag::Msg`/`Hint` at construction time, for the
CLI's own terminal output. Passed through uncleaned, every LSP diagnostic
message for one of these would show literal `\x1b[...m` garbage in the editor.
`SLMIntegration.cpp` already strips these before handing error text to the SLM
prompt for the identical reason; `CompilerService.cpp`'s `stripAnsiCodes()`
does the same on the LSP side (deliberately not "fixed" at the source — colors
are baked in by design for CLI display, and stripping is the consuming side's
job, matching the precedent `SLMIntegration.cpp` already set).

## Acceptance tests
- ✅ Reference an undeclared variable → error with the correct `E0NN` code and a
  token-wide squiggle. (`diagnostics_test.py`)
- ✅ Two errors on one physical line both appear (proves `setCollectAllDiagnostics`)
  — verified with two **parse-level** errors specifically; two resolver-level
  errors (e.g. two undeclared names) do NOT both appear, because
  `resolveAll()`/`checkAll()` each still recover as a single unit for the whole
  pass (a `raise()` deep inside longjmps out of the entire call, not just the
  current statement) — a separate, pre-existing compiler recovery-granularity
  constraint this task doesn't claim to fix. (`diagnostics_test.py`)
- ✅ Fixing the last error clears the squiggles (empty publish arrives).
  (`diagnostics_test.py`)
- ✅ A file with `é` before the error still squiggles the correct columns.
  (`diagnostics_test.py`, `phase0_test.py`)
- ⬜ "Type `let x = ` (incomplete) → a diagnostic appears on that statement
  only... no diagnostics leak onto later lines" — not separately verified;
  follows from the parse-recovery behavior already exercised elsewhere, but
  wasn't given its own test this session.
- ⬜ "An error whose `RelatedLocations` point into an imported module renders a
  clickable related entry" — the plumbing is in place (P1.4) and
  `diagnostics_test.py` checks the shape of any `relatedInformation` that
  shows up, but no source triggering a real cross-file `RelatedLocation` was
  found/constructed this session to exercise it end-to-end.
