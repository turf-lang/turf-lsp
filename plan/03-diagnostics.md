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
- **P1.1** Set `setCollectAllDiagnostics(true)` around analysis; keep
  `g_AIEnabled=false`.
- **P1.2** Re-enable publishing: map `Diag`→`Diagnostic` with `code`,
  `codeDescription`, `source`, `relatedInformation`, hint-in-message.
- **P1.3** Real end-columns (doc 02 §E) with buffer-widen fallback.
- **P1.4** Multi-URI publish + clear-on-clean + version tagging.
- **P1.5** Field-test false positives; add the minimal syntax-error gating only
  if needed.
- **P1.6** (later) Optional pull-model `textDocument/diagnostic`.

## Acceptance tests
- Type `let x = ` (incomplete) → a diagnostic appears on that statement only,
  code shown, and clears when completed — no diagnostics leak onto later lines.
- Reference an undeclared variable → error with the correct `E0NN` code and a
  token-wide squiggle.
- Two errors on one physical line both appear (proves `setCollectAllDiagnostics`).
- An error whose `RelatedLocations` point into an imported module renders a
  clickable "related" entry that opens that file at the right spot.
- Fixing the last error clears the squiggles (empty publish arrives).
- A file with `é` before the error still squiggles the correct columns.
