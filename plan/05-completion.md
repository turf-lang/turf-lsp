# 05 — Completion (auto-complete, imports, functions, members)

## Goal
Context-aware suggestions covering the three requested cases explicitly:
- **Auto-complete suggestions** (identifiers/keywords/types in scope),
- **Module import suggestions** (`import <here>` — module names + `import
  mod::<symbol>`),
- **Function suggestions** (free functions, module functions, methods, with
  call-snippet insertion).

## LSP surface
- `textDocument/completion` (have) + `completionItem/resolve` (add — for lazy
  documentation/detail).
- Trigger characters: `.` and `:` (have). `::` needs care (see below).
- Optional stretch: `textDocument/signatureHelp` (param hints while typing a
  call) — same symbol data; see end.

## Current state (already solid)
`SemanticQuery::getCompletions()` classifies the trigger purely textually
(`classifyTrigger`) into `ScopeAccess` (`Ident::`), `MemberAccess` (`instance.`),
or `Bare`, then answers from the resolved symbol table:
- `Ident::` → module functions/types/constants, enum variants (+ synthesized
  `COUNT/VALUES/NAMES`), or struct static methods + `new`/`default`.
- `instance.` → struct fields + instance methods (via the flat
  `StructName.method` naming convention).
- Bare → keywords, primitive types, document variables, current-file functions,
  struct/enum names, `assert`, literals.
- Snippets via `buildCallSnippet` (`method(${1:arg})$0`), skipping `self`.

It also patches the buffer (`patchForCompletion` → `turfLspCursor;`) so a
dangling `io::`/`p.` still parses the enclosing function, and runs a dedicated
analysis for the request.

## Gaps to close

### G1 — Reuse the cached analysis; patch only when needed — ✅ PARTIALLY DONE
**Done (2026-09-15, reworked same day once doc 01's DocumentStore landed).**
`doCompletion()` now goes through `ensureFreshAnalysis()` (doc 01 P0.2) — the
real `DocumentStore`-backed cache, not the original ad-hoc read of
`m_documents[uri].analysis` — for `Ident::` triggers specifically, falling
back to the patched reanalysis only if that comes up empty. Scoped narrower
than originally planned here: a `::` base is always a module/struct/enum
name, which registers from its own separate top-level declaration
independent of the current (possibly still-being-typed) statement's
enclosing function — so reuse is provably safe for that one trigger shape.
Going through `ensureFreshAnalysis()` (rather than a raw cached pointer) also
matters now for a reason that didn't exist when this optimization first
landed: analysis is asynchronous and debounced (doc 01 P0.1) instead of
synchronous-per-didChange, so a naive cached pointer could be stale relative
to the document's current text at the moment a completion request actually
runs; `ensureFreshAnalysis()` re-analyzes inline if so, guaranteeing
correctness without needing to coordinate with the debounce timer.

**Deliberately NOT extended to Bare or `.` (MemberAccess) triggers** — tried a
broader "reuse whenever the cached result is non-empty" version first and
caught it regressing live: a bare-word completion mid-typing (no trailing `;`
yet) returned keywords/types only, silently missing the in-scope local that
should have been in the list. `getCompletions()`'s Bare branch always pads
the list with keywords/types, so "non-empty" is *not* a valid signal that
locals are complete — turf-lang's recovery only resyncs at top-level
statement boundaries, so an unterminated statement anywhere in a function
drops that whole function's locals from the cached (unpatched) analysis.
Bare and `.` still always run the patched reanalysis, unchanged from before.
A real per-trigger scope-awareness fix (only reuse the cache when the
specific local a MemberAccess/Bare completion needs is provably unaffected)
is future work, not attempted here.

### G2 — Import context (the missing requested feature)
Add a 4th trigger context: **Import**. Detect when the current line matches
`import <prefix>` or `from <prefix>` or `import mod::<prefix>`:
- `import <prefix>` → suggest **module names**. Source the list from the
  compiler's module index (doc 02 §A `resolveModuleFile` sibling: expose
  `listAvailableModules(turfHome, importingFile)` that returns all stdlib module
  names from `IndexModuleDir` + user modules found next to the file / in
  `lib/usr`). Kind = `Module` (9). Insert the bare name + `;`.
- `import mod::<prefix>` → suggest the **exported symbols** of `mod`
  (functions/types/constants) using the existing module-symbol queries. This is
  the "module import suggestions" requirement in its precise form.
- `from mod import <prefix>` (if that syntax exists — confirm against
  `ParserModule.cpp`; `from` is a keyword per SYNTAX_REFERENCE) → same symbol
  list.

Advertise `import`/`from` completion without requiring a trigger char (invoked
completion), since these are typed as words.

### G3 — Scope-precise filtering
`DocumentIndexVisitor` collects **all** variable declarations flat (its comment
admits over-suggesting out-of-scope vars). Improve ranking, not correctness:
- Prefer variables whose declaration precedes the cursor and whose enclosing
  block contains the cursor. Use the cursor position + node ranges to compute a
  rough scope-distance and sort by it (closer scope first). Keep out-of-scope
  ones but rank them lower (a completion list tolerates extra entries; missing a
  real one is worse — keep that stance).
- De-duplicate labels (same name from multiple scopes → one entry).

### G4 — `completionItem/resolve` for lazy detail
Advertise `resolveProvider: true`. Return lightweight items first (label + kind +
short detail); on resolve, fill `documentation` (doc-comment of the function,
full signature, module origin) so the initial list stays fast. Carry an opaque
`data` field (symbol id) on each item to resolve it.

### G5 — Prefix filtering & fuzzy matching
The server currently returns the full candidate set and lets the client filter.
That's valid (VS Code filters client-side), but for large candidate sets return
`isIncomplete: true` and pre-filter by the typed `prefix` (already captured in
`TriggerContext.prefix`) using the compiler's own `SuggestionIndex` trie
(`include/SuggestionIndex.h`) for fuzzy/edit-distance ranking — reusing the same
"did you mean" engine the error messages use keeps suggestions consistent with
diagnostics.

### G6 — Richer kinds & sorting
- Set `sortText` so locals/params rank above module functions above keywords.
- Add `Field` vs `Method` vs `Property` kinds correctly (mostly done).
- For functions, keep the snippet insert; also set `labelDetails` (signature) for
  a cleaner list in modern clients.

## Trigger-character handling for `::`
LSP fires the trigger on a single `:`; the first `:` of `::` will invoke
completion prematurely. Handle by: when triggered on `:` and the char before is
not also `:`, return an empty/incomplete list (wait for the second colon). When
the classifier sees `Ident::`, answer scope completions. Keep `.` immediate.

## Signature help (stretch, same data)
When inside a call's argument list (`foo(|` or after a `,`), show the resolved
overload's `signature()` with the active parameter highlighted. Reuses
`ResolverFunctionSymbol.Params`. Advertise `signatureHelpProvider` with triggers
`(` and `,`. Nice-to-have; schedule after M2.

## Edge cases
- Completion inside a comment or string literal → return nothing (detect via the
  lexer state or a cheap textual check of the prefix context).
- `self.` inside a method → resolve `self`'s struct type and offer its
  fields/instance methods (verify `self` is in `VariableTypes`; if not, special-
  case it from the enclosing `FuncDefExprAST`'s owner struct).
- Chained member access `a.b.c` → today only single-level `base.` is handled
  (`DocumentIndexVisitor` maps var→type). Extend to resolve the type of `a.b`
  (field type) to offer `c`. Requires walking field types through struct decls;
  do it iteratively over the dotted chain in the prefix.
- Unknown/partial base → empty list, never a crash.
- Multibyte prefix → prefix extraction is byte-wise but only over identifier
  chars (ASCII), so safe; the *insert position* still goes through
  `PositionMapper`.

## Phased tasks
- **P2.1** Reuse cached analysis; patch-parse only when necessary (G1).
- ✅/⬜ **P2.2** Partially done (2026-09-15): `import <prefix>` module-name
  completion shipped, backed by turf-lang's new `ListAvailableModules()`
  (unions `lib/std/**` + `lib/usr/**` + sibling `.tr` files, sharing
  `IndexModuleDir()`/`TurfInstallRoot()` with `ResolveModuleFile()` so it
  can't disagree with a real import). Confirmed turf-lang has no `from X
  import Y` form to also support (only `extern from "lib" fn ...` uses
  `from`). Not done: `import mod::<prefix>` (mod's exported symbols) --
  needs loading the target module's own symbol table before it's actually
  imported into this file, a meaningfully bigger feature than the
  filesystem listing above; genuinely deferred, not skipped for
  convenience.
- **P2.3** `completionItem/resolve` + `data` payload + lazy docs (G4).
- **P2.4** Scope-distance ranking + de-dup + `sortText` (G3, G6).
- **P2.5** Prefix pre-filter via `SuggestionIndex` trie; `isIncomplete` for big
  sets (G5).
- **P2.6** Chained member access + `self.` (edge cases).
- **P3.7** Signature help (stretch).

## Acceptance tests
- ✅ Typing `import ` lists real module names incl. those in subfolders
  (`net`, `list`, `io`, ...); accepting inserts `import net;` -- verified
  live (2026-09-15): `import ne` returns all 27 resolvable names
  (23 stdlib + sibling files), each with insertText `"<name>;"`.
- ⬜ `import io::` lists `io`'s exported functions -- not done, see P2.2.
- `p.` on a `Point` instance lists its fields then instance methods (not
  statics); `Point::` lists `new`, `default`, static methods (not instance).
- A free function suggestion inserts a call snippet with tab-stops over its
  params (self skipped for methods).
- Completing does **not** trigger a second full analysis (perf: reuses cache).
- Inside a string literal, no completions appear.
- `Color::` lists variants + `COUNT/VALUES/NAMES`.
