# 10 — Refactoring (rename + code actions)

## Goal
Safe symbol rename across the workspace, and code actions covering both
diagnostic quick-fixes and common refactors.

## LSP surface
- `textDocument/prepareRename` → the renameable range (or reject).
- `textDocument/rename` → `WorkspaceEdit` (edits across files).
- `textDocument/codeAction` → `CodeAction[]` (quickfix / refactor / source).
- `codeAction/resolve` → fill in the `edit` lazily for expensive actions.

## Current state
None. Rename depends entirely on **find references** (doc 08) and stable symbol
identity (doc 02 §C) being correct; code actions depend on **diagnostics**
(doc 03) carrying error codes.

## Design

### Rename (reference-driven, must be safe)
Rename is find-references + text replacement, but with strict safety:
1. `prepareRename`: `findNodeAt(cursor)` → must be a renameable symbol
   (variable, param, function, method, struct, enum, enum member, module alias).
   Reject (return null / error) on keywords, literals, builtins (`assert`),
   primitive types, and anything with a `{0,0}`/synthetic decl. Return the exact
   name range (via `NameLoc`, doc 02 §B — **required** here; a textual span could
   rename the wrong token).
2. `rename(newName)`:
   - Validate `newName` is a legal Turf identifier and **not a reserved keyword**
     (reuse the lexer's keyword set) — reject otherwise with a clear message.
   - Compute the symbol's `symbolId` (doc 02 §C) and gather **all** occurrences
     from the workspace occurrence index (doc 08), including the declaration.
   - Build a `WorkspaceEdit` with per-file `TextEdit[]` replacing each occurrence
     span with `newName`.
   - **Conflict checks**: refuse (or warn) if `newName` would collide with an
     existing declaration visible in any affected scope (shadowing/redefinition),
     or clash with a stdlib/module name. Better to reject a risky rename than
     silently break code.
3. Never rename across a symbol boundary: same-name-different-symbol occurrences
   must be excluded (identity, not text — the whole reason doc 02 §C exists).

**Scope of rename**: local variables → within their function only (their id is
`file+DeclLoc`, so occurrences are naturally file-local). Exported
functions/types → workspace-wide. **Renaming stdlib symbols**: reject by default
(the user can't edit stdlib meaningfully, and it would desync the toolchain);
allow only if the decl is in a workspace (user) file.

### Code actions

**A. Diagnostic quick-fixes** (join on the diagnostic `code`, doc 03). Build a
table keyed by `E0NN`:
| Code (example) | Quick-fix |
|---|---|
| Unknown module / unresolved `x::` | "Add `import x;`" (insert after existing imports) |
| Unknown identifier w/ near-match | "Change to `<suggestion>`" using `SuggestionIndex` (the compiler already computes "did you mean") |
| Unused variable/import (a lint warning) | "Remove unused `x`" |
| Missing `;` / trivial syntax | "Insert `;`" |
| Immutable assignment (`immut`) | "Change `immut` to `mut`" |
| Type mismatch with an obvious cast | "Add `to <Type>`" cast |

The exact set depends on which `E0NN` codes exist (`docs/errors/codes/E001..E068`
+ their `.txt` explanations). **Task 0 for this doc: enumerate the 68 codes and
pick the ones with mechanical fixes.** The compiler's existing `Hint` and
`SuggestionIndex` output often already contains the fix text — reuse it rather
than re-deriving.

**B. Refactors** (`refactor.*`):
- **Extract variable**: selection is an expression → introduce `immut tmp: T =
  <expr>;` above and replace the selection with `tmp`. Needs the expression's
  resolved type for the annotation (available from the type checker).
- **Extract function**: selected statements → new `fn` with inferred params
  (free variables read) and return (value produced). More complex; schedule
  after extract-variable.
- **Inline variable**: reverse of extract, for a single-assignment local.
- **Organize/add imports** (`source.organizeImports`): sort import lines, remove
  unused imports (needs "is this import referenced" from the reference index),
  add missing ones for used-but-unimported module symbols.
- **Change `mut`↔`immut`**: toggle mutability on a variable decl (safe only if no
  writes exist for immut→ok, or making mut always safe).

**C. Source actions** (`source.*`): `source.formatDocument` (delegate to doc 09),
`source.organizeImports`.

Use `codeAction/resolve` so the list is cheap to produce (labels + kinds) and the
`WorkspaceEdit` is computed only when the user picks one (extract/organize can be
expensive).

## Edge cases
- Rename with the same name (`newName == oldName`) → no-op empty edit.
- Rename a struct that has a `new`/`default`/methods → those are stored under
  `StructName.method`; renaming the struct must update those flat names'
  *display* references (call sites `Struct::method`, `instance.method` keep the
  method name; only the `Struct` part changes). Ensure the id scheme distinguishes
  the type name from its methods so only type-name occurrences change.
- Rename a module alias vs the module file name — renaming an `import` alias
  (if aliases exist) only touches the alias uses, not the file.
- Multi-file edits where one target file is unopened → `WorkspaceEdit` may edit
  files on disk; ensure the client is asked to apply (VS Code shows a preview).
- Code action on a buffer with many diagnostics → only offer fixes for
  diagnostics overlapping the requested range/position.
- Conflicting/overlapping edits → dedupe and order; never produce overlapping
  `TextEdit`s.

## Phased tasks (Phase 4)
- ✅ **P4.1** Done (2026-09-16): `prepareRename` -- renameable check +
  exact range, reusing `hover()`'s per-node-kind span (not `NameLoc`
  directly, but the same underlying data).
- ✅/⬜ **P4.2** Done, scoped down: `rename` uses the workspace occurrence
  scan (shared with `references`, doc 08) + identifier/keyword
  validation + stdlib guard. The deeper "would `newName` collide with
  an existing declaration visible in scope" conflict check is NOT
  implemented -- needs a real per-scope visibility query this session's
  identity model (declLoc + module) doesn't answer; a genuinely bigger
  feature, not attempted rather than faked.
- ✅/⬜ **P4.3** Done, scoped to 2 fixes (2026-09-16), not the full 68-code
  enumeration: "Add import" (E032, a real coded join) and "Prefix
  unused var with underscore" (message-pattern matched -- warnings never
  carry a code at all, confirmed by reading `TurfWarning::warn()`).
- ✅ **P4.4** Done: `codeAction` implemented, computed eagerly (no
  `codeAction/resolve` needed for either fix -- both are cheap).
- ⬜ **P4.5** Not done -- extract variable, organize imports.
- ⬜ **P4.6** Not done (explicitly "later" even in the original plan).

**Real gap found while testing, not fixed (out of scope for this doc,
belongs on doc 03)**: `CompilerService::analyze()` never runs the
CFG-based lint pass (`UnusedVariableWarning`, dead-store, etc. -- all in
`Analysis.cpp`'s `LivenessAnalysisPass`, invoked from Codegen, a phase
the LSP intentionally never runs). This means `publishDiagnostics` never
actually surfaces this entire class of warning today, even though the
real `turf` CLI does. Confirmed live: a genuinely unused variable
produces an empty diagnostics array through a real `didOpen`, but
reproduces the warning correctly through `turf --typecheck` on the
identical source. The "prefix with underscore" quick-fix above is
correct and tested (via a synthetic diagnostic matching the warning's
real message format) but is presently unreachable in practice until
this is fixed -- it will start firing the moment a future session closes
the gap.

## Acceptance tests
- ✅ Rename a function used in 3 files updates all call sites + the def, in one
  `WorkspaceEdit`; unrelated same-named locals are untouched -- verified live.
- ✅ Rename to a keyword (`fn`) is rejected with a clear message -- verified live.
- ✅ Rename a local doesn't touch a same-named local in another function --
  inherited directly from doc 08's identical, already-verified shadowing test
  (same identity model, same underlying scan).
- ⬜ `prepareRename` on `assert`/`int` is rejected -- not separately
  tested this session (should already hold: `assert` has no resolved
  declaration at all per doc 07's existing "builtin" handling, and a
  bare `int` type-name use isn't a node kind `getDeclarationLoc()`
  resolves in the first place -- structural, not verified live).
- ✅ Quick-fix on "unknown module"-shaped error offers "Add `import
  X;`" and inserts it -- verified live with E032 specifically
  (`UnqualifiedTypeNotVisibleError`, the closest real analogue to the
  doc's own "unknown module `io`" example): applying the returned edit
  makes the file compile cleanly through the real `turf` CLI.
- ⬜ Did-you-mean quick-fix -- not implemented (the suggestion format has
  multiple ranked candidates across local/module sources with no single
  unambiguous replacement to auto-pick; judged too fragile to parse
  reliably, not attempted rather than shipped fragile).
- ⬜ Extract-variable acceptance test -- moot, extract-variable not
  implemented (P4.5).
- ✅ Rename of a stdlib symbol is refused by default -- verified live on
  a real stdlib function (`net::buildSockAddr`), both `prepareRename`
  (returns null) and `rename` (returns a clear error).
