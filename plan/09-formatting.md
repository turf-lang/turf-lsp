# 09 — Code formatting

## Goal
Deterministic, idempotent formatting of Turf source: whole-document,
range, and on-type (e.g. re-indent on `}`). Format-on-save in both editors.

## LSP surface
- `textDocument/formatting` → `TextEdit[]` (whole doc).
- `textDocument/rangeFormatting` → `TextEdit[]` (selection).
- `textDocument/onTypeFormatting` → `TextEdit[]` (after a trigger char).
- Capabilities in doc 01 §8. Honor `FormattingOptions` (`tabSize`,
  `insertSpaces`, `trimTrailingWhitespace`, `insertFinalNewline`).

## Current state
**No formatter exists** anywhere in `turf-lang` or the LSP. This feature builds a
new engine (doc 02 §D) in the compiler, exposed as `turf --format` **and** as a
library function reused in-process by the LSP — one implementation, two callers.

## Why the engine lives in the compiler
- It needs the real lexer (for exact tokens + trivia/comments) and ideally the
  AST (for structural decisions). Both already exist in `turfcore`.
- CLI users want `turf --format` regardless of the LSP.
- A separate LSP reimplementation would drift from the language grammar (the
  exact bug class `AGENTS.md` warns about repeatedly).

## Design

### Approach: token/AST hybrid, comment-preserving
A pure AST pretty-printer (there is an `ASTPrinter.h`, but that's a *debug* dump,
not a faithful formatter — it does not round-trip comments/trivia). Instead:

1. **Lex the full source into a token stream that retains trivia** (whitespace
   runs, blank lines, and comments as attached leading/trailing trivia). The
   lexer currently discards comments; add a "preserve trivia" mode (or a
   companion trivia lexer) used only by the formatter. This is the critical
   design constraint: **comments and meaningful blank lines must survive.**
2. **Parse to AST** (recovery on) to know structure (blocks, decls, call
   argument lists, control flow) for indentation and line-breaking decisions.
3. **Render** by walking the AST and re-emitting tokens with normalized
   whitespace, re-attaching each node's leading/trailing trivia.

If full AST-driven rendering proves too large initially, ship a **token-stream
formatter** first (indentation by brace depth, one statement per line, normalized
spacing around operators/punctuation, comment passthrough). It handles ~90% of
value and is much simpler; upgrade to AST-driven line-breaking later. Recommend
starting token-stream, evolving to hybrid.

### Formatting rules (initial style — derive from `showcase/` + `tests/`)
Match the existing hand-written style so diffs are minimal:
- **Indentation**: 4 spaces per block level (showcase uses 4). Respect
  `FormattingOptions.tabSize`/`insertSpaces` overrides.
- **Braces**: K&R — `fn f(): int {` on the same line; closing `}` on its own
  line at the opening statement's indent.
- **Spacing**: one space around binary operators and `=`; no space before `(` in
  calls; one space after `,`; `: ` in type annotations (`name: Type`); `::` and
  `.` tight; space after keywords (`if `, `while `, `for `, `return `).
- **Blank lines**: collapse >1 consecutive blank line to 1; one blank line
  between top-level declarations; no leading/trailing blank lines (respect
  `insertFinalNewline`).
- **Trailing whitespace**: strip (respect `trimTrailingWhitespace`).
- **Line length**: initially no hard wrapping of long calls (safer/idempotent);
  add argument-per-line wrapping past a width later, behind an option.
- **Comments**: preserve verbatim, re-indented to the surrounding code level;
  never reflow comment text.

Encode rules in a `FormatOptions` struct so CLI flags and LSP
`FormattingOptions` map onto the same knobs.

### Idempotence (hard requirement)
`format(format(x)) == format(x)` for all inputs. Enforce with golden tests
(doc 02 §D). Idempotence failures are the most common formatter bug — test every
`showcase/*.tr` and a broad `tests/**` sample.

### Producing `TextEdit[]`
- Simplest correct: format the whole doc, diff old vs new text (line-based or
  Myers), emit minimal `TextEdit`s. A single full-replace edit is *allowed* but
  churns the client's cursor/undo; prefer a minimal diff. Reuse a small diff util
  (or the compiler's `ASTDiff` is unrelated — this is a text diff).
- **rangeFormatting**: format only the selected statements/block. Safest impl:
  format the whole file, then restrict emitted edits to those overlapping the
  requested range (guarantees consistency with full-format). If that reflows
  outside the range, fall back to formatting just the enclosing block.
- **onTypeFormatting**: on `}` re-indent the just-closed block; on `;`/newline
  normalize the current line's indentation. Keep these edits tiny and local.

### Never format an unparseable buffer destructively
If the buffer has parse errors severe enough that structure is unknown, either
(a) format only the token stream conservatively, or (b) return no edits. Never
emit edits that could delete code. Prefer returning no edits over a risky
reformat on a broken buffer.

## Edge cases
- Buffers with only comments / empty files → return no-op or just
  final-newline/trailing-space normalization.
- Mixed tabs/spaces input → normalize to the requested indent unit.
- Strings/char literals containing brace or `//` sequences → the trivia-aware
  lexer must not treat those as structure/comments.
- CRLF vs LF → preserve the document's dominant EOL (or normalize to LF behind an
  option; default preserve).
- Very large files → formatting is O(n); fine, but run on the worker thread like
  everything else.
- Multibyte content → edits' ranges via `PositionMapper`.

## Phased tasks (Phase 4)
- ✅/⬜ **P4.1** Done, scoped down (2026-09-16): `turf::format::format()` +
  `turf --format[ --write]` shipped, but WITHOUT the trivia-preserving
  lex mode this task originally called for. Turned out unnecessary for
  an indentation-only formatter: re-indenting only touches each line's
  LEADING whitespace, using the compiler's existing `g_braceDepth`
  counter (already tracked globally by `getNextToken()` for parse-error
  recovery) for depth, never re-tokenizing/re-emitting comments or
  trivia at all -- so nothing needed to change in the Lexer.
- ✅/⬜ **P4.2** Scoped down: indentation-only, not the fuller
  spacing/operator/blank-line rule set doc 09's design section lists
  (K&R braces, operator spacing, etc.) -- deliberately deferred, see
  Formatter.h's own doc comment for why (avoids the `List<T>` vs `a < b`
  generic-bracket ambiguity a token-stream RE-EMISSION would have to
  solve correctly). Idempotence + "still compiles" verified exhaustively
  (see the commit) but not added as a permanent `turf-lang/tests/`
  golden-file suite -- ad hoc verification only this session.
- ✅ **P4.3** Done: `textDocument/formatting`, single full-document
  `TextEdit` (spec-legal, not the nicer minimal diff -- explicitly
  allowed, see the commit).
- ⬜ **P4.4** Not done -- `rangeFormatting`.
- ⬜ **P4.5** Not done -- `onTypeFormatting`.
- ⬜ **P4.6** Not done (explicitly "later" even in the original plan).

## Acceptance tests
- ✅ Formatting every `showcase/*.tr` is idempotent and the result still compiles
  (`test_runner` semantics) -- verified live for all 7, plus all 645 `tests/**/*.tr`
  (100% idempotent; the real `test_runner.sh` against the whole `tests/`
  tree formatted in place gives the identical 605/606 baseline).
- ✅ Mangled-whitespace input (random spaces/blank lines) formats to the
  canonical style -- verified live.
- ✅ A file with dense doc-comments keeps every comment, correctly
  re-indented -- verified live, including the "comment directly after an
  opening brace" and "comment directly before a closing brace" edge
  cases specifically (both caught real bugs during testing, both fixed
  before landing).
- ⬜ `rangeFormatting` a single function leaves the rest of the file
  byte-identical -- not implemented (P4.4).
- ⬜ Format-on-save works in VS Code and Neovim -- not verified (no live
  editor session available this session, same limitation doc 03's P1.5
  already notes).
- ✅ Formatting a buffer with a syntax error either no-ops or
  conservatively formats without deleting code -- structurally true by
  construction (the formatter only ever rewrites leading/trailing
  whitespace per line; it cannot delete a non-whitespace byte regardless
  of whether the buffer parses), not separately tested against a live
  syntax-error buffer this session.
