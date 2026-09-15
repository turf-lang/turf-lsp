# 08 — Find references (+ document highlight, document/workspace symbols)

## Goal
From any symbol, list every place it's used across the workspace; highlight
same-symbol occurrences in the current file; enumerate symbols in a file / the
workspace.

## LSP surface
- `textDocument/references` → `Location[]` (respect
  `context.includeDeclaration`).
- `textDocument/documentHighlight` → `DocumentHighlight[]` (in-file, kinds
  Read/Write/Text).
- `textDocument/documentSymbol` → outline of the current file (bonus, cheap,
  reuses the same index).
- `workspace/symbol` → fuzzy symbol search across the workspace (bonus).

## Current state
None implemented. This is the first feature that is genuinely **multi-file** and
requires the `WorkspaceIndex` (doc 01 §7) and stable symbol identity (doc 02 §C).

## Design

### Symbol identity (the crux)
References = "all occurrences whose resolved target has the same identity as the
symbol under the cursor." Correctness hinges on **identity**, not name text:
- Two locals named `x` in different functions are different symbols.
- A method `Point.distance` and a free `distance` are different.
- The same imported function used in two files is one symbol.

Use compiler item C (doc 02 §C1): each resolved declaration and each use-site
exposes a stable `symbolId()`:
- Functions/methods/types: mangled `SourceModule + BaseName + ParamTypes` (already
  the internal identity per `ResolverSymbolBase.h`).
- Locals/params: `declaringFileUri + DeclLoc` (unique per declaration site).

Until item C lands, a **name+scope heuristic** can ship an in-file-only
`documentHighlight` (match resolved-symbol pointer identity within one
analysis — the LSP already has `getResolvedSymbol()`), but true cross-file
`references` should wait for stable ids to avoid false matches.

### Building the occurrence index
`WorkspaceIndex` maintains, per file, an `OccurrenceTable`:
```cpp
struct Occurrence { SymbolId id; LspRange range; bool isDecl; RefKind kind; };
// kind ∈ {Read, Write}  (Write when the occurrence is an assignment LHS)
std::unordered_map<std::string /*fileUri*/, std::vector<Occurrence>> perFile;
std::unordered_multimap<SymbolId, {fileUri, index}> bySymbol; // reverse index
```
Population: for each `.tr` file, run the cached analysis, walk the AST with a
`ReferenceVisitor` that, for every name-bearing node (variable, call, method,
field, type name, module name, enum member), records `{symbolId(node), NameLoc,
isDecl, kind}`. `NameLoc` (doc 02 §B) gives exact spans; the current textual
`findNameLoc` is the interim fallback.

Population is **lazy + incremental**:
- Index the open file eagerly (already analyzed).
- Index other workspace files on first `references`/`workspace/symbol` request,
  in the background, cached.
- Invalidate a file's occurrences on its didChange/save/watched-file-change.
- Skip `build/`, `.cache/`, hidden dirs; cap file size; honor `TURF_HOME` for
  stdlib (index stdlib lazily only when a reference target lives there).

### references
1. `findNodeAt(cursor)` → node → `symbolId`.
2. Ensure all candidate files are indexed (a symbol defined in module M can be
   referenced anywhere that imports M → for a workspace-wide answer, index files
   that transitively import the defining module; simplest correct approach:
   index all workspace `.tr` files, which is fine for typical project sizes).
3. `bySymbol.equal_range(symbolId)` → collect ranges; filter out the declaration
   if `includeDeclaration == false`.
4. Return `Location[]`.

### documentHighlight
Same as references but restricted to the current file (no workspace scan needed);
tag each occurrence Read vs Write (assignment LHS → Write). Fast path off the
already-cached analysis.

### documentSymbol / workspace/symbol (bonus, cheap reuse)
- `documentSymbol`: emit a hierarchical outline (structs with their
  fields/methods nested, enums with variants, top-level functions, globals) from
  the file's AST. Uses `SymbolKind` (Function=12, Struct=23, Enum=10,
  Field=8, EnumMember=22, Variable=13, Method=6, Module=2).
- `workspace/symbol`: fuzzy-match a query against all declaration names in the
  index (reuse `SuggestionIndex` trie for ranking).

## Edge cases
- Shadowing: an inner `x` shadowing an outer `x` — identity by `DeclLoc` keeps
  them distinct; references on the inner must not include the outer's uses.
- Struct field vs local of the same name — distinct ids (field id includes the
  struct).
- References to a stdlib symbol from user code: include user occurrences; whether
  to include occurrences *inside* stdlib is a policy choice — default: include
  workspace files only, optionally include stdlib if the user opened it / setting
  enabled.
- Unresolved symbol under cursor (mid-edit) → empty result.
- Very large workspace → cap and stream; return `isIncomplete`-style partial via
  progress (`$/progress` with a work-done token) rather than blocking.
- File with parse errors → still index the parts that resolved; partial refs are
  better than none.

## Phased tasks (Phase 3)
- ✅ **P3.1** Done (2026-09-16), via the doc's own escape hatch: shipped
  on a name+scope identity heuristic -- (DeclLoc, module resolved to an
  absolute file path) -- instead of waiting on compiler item C's
  `symbolId()`. Turned out sufficient for workspace-wide `references`
  too, not just in-file `documentHighlight`; item C not attempted.
- ✅ **P3.2** Done, scoped down: `ReferenceVisitor` +
  `findAllReferenceOccurrences()` exist and cover variable use/decl,
  function call/def, method call (the 5 kinds `getDeclarationLoc()`
  already resolves) -- struct/enum/field/module-name occurrences are a
  documented scope cut. No persistent `OccurrenceTable` wired into
  `WorkspaceIndex` -- see P3.4.
- ✅ **P3.3** Done: in-file, restricted by matching identity. Read/Write
  tagging not implemented (needs one more bit `ReferenceVisitor` doesn't
  compute yet -- real scope cut, not attempted).
- ⬜/✅ **P3.4** `WorkspaceIndex` file enumeration reused as-is (doc 01
  P0.6, already existed); no lazy indexing/caching or file-watcher
  invalidation added -- `references` re-analyzes every candidate file
  fresh on EVERY request instead. Correct, not incremental; acceptable
  per the doc's own "fine for typical project sizes" allowance, revisit
  if real usage says otherwise.
- ✅ **P3.5** Done: workspace-wide, `includeDeclaration` honored,
  verified across a real 3-file workspace.
- ✅/⬜ **P3.6** `documentSymbol` done (flat/non-hierarchical -- top-level
  struct/enum/function only, no nested fields/methods). `workspace/symbol`
  not done.
- ⬜ **P3.7** Not done -- no `$/progress` reporting; a large-workspace
  `references` scan simply blocks until it finishes.

## Acceptance tests
- ✅ References on a function used in 3 files returns all call sites + the decl
  (and excludes the decl when `includeDeclaration=false`) -- verified live.
- ✅ Two locals named `i` in different functions: references on one returns only its
  own uses -- verified live.
- ⬜ `documentHighlight` on a variable marks its assignment as Write and reads as
  Read -- not implemented (see P3.3).
- ⬜ References on a struct field returns only that struct's field uses, not
  same-named locals -- not implemented (field occurrences out of scope, see P3.2).
- ⬜ Editing a file updates its occurrences without re-indexing the whole workspace
  -- moot: there's no persistent index to invalidate (see P3.4); every request
  re-scans fresh, so edits are always reflected, just not incrementally.
- ✅/⬜ `documentSymbol` shows top-level functions (verified); structs with
  nested fields/methods not implemented (flat outline only).
