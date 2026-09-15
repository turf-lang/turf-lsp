# 04 — Code highlighting (semantic tokens + TextMate grammar)

## Goal
Correct, meaning-aware coloring: keywords, types, functions vs methods vs
fields, module namespaces, enum members, parameters, mutable vs `immut`
(readonly), etc. Two layers working together:
1. **TextMate grammar** — instant, regex-based, works before/without the server
   (and colors the file the moment it opens).
2. **Semantic tokens** — precise, AST-driven, refines the grammar's guesses
   (e.g. distinguishing a struct type from a variable, a method from a field).

## LSP surface
- `textDocument/semanticTokens/full` (have), `.../full/delta` (add),
  `.../range` (add, for large files).
- `semanticTokensProvider.legend` must list every token type/modifier used.

## Current state
- `SemanticTokensVisitor` (`src/SemanticQuery.cpp`) emits tokens for variables,
  var/func/struct/extern decls, calls, methods, fields, scope access, module
  imports, arrays. Delta encoding is computed in `handleSemanticTokens`.
- Legend advertised in `handleInitialize`: `variable, function, keyword, type,
  parameter, struct, string, number, operator, namespace, property, enumMember,
  method` + modifiers `declaration, readonly`.
- TextMate grammars exist in **two** places (`editors/vscode/syntaxes/` and
  `vscode-turf/syntaxes/`) plus a `grammars/` dir in turf-lang — consolidate
  (doc 11).
- Relies on textual `findNameLoc()` for name spans → replace with real
  `NameLoc` from compiler item B (doc 02 §B) once available.

## Design

### Layer 1 — TextMate grammar (`turf.tmLanguage.json`)
Owns fast, purely-lexical coloring and stands alone when the server is down.
Cover from `SYNTAX_REFERENCE.md` + real syntax in `showcase/` and `tests/`:
- **Keywords**: `fn mut immut struct enum extern from import export static if
  elseif else while for in step give otherwise break continue return to` (see
  the completion keyword list in `SemanticQuery.cpp` and
  `turf-lsp/SYNTAX_REFERENCE.md`).
- **Types**: `int int32 double bool string char ptr void` + generics `List<T>`,
  `Set<T>`, `Map<K,V>` (from showcase). Capitalized identifiers as types
  (heuristic).
- **Literals**: numbers (int/double), strings with escapes, `true false null`.
- **Comments**: `//` line comments (block comments? confirm from lexer — grep
  `Lexer.cpp`; if none, don't invent).
- **Operators & punctuation**: `:: . -> :` , casts via `to`.
- **Function/struct/enum names** at declaration and call sites (best-effort).

Keep the grammar accurate but *shallow* — semantic tokens correct its mistakes.
Validate the grammar against every `showcase/*.tr` visually.

### Layer 2 — Semantic tokens (authoritative refinement)
Extend the existing visitor. Target coverage and token-type mapping:

| Construct | tokenType | modifiers |
|---|---|---|
| Local/param variable use | `variable` / `parameter` | `readonly` if `immut` |
| Variable/array declaration name | `variable` | `declaration`(+`readonly`) |
| Function definition name | `function` | `declaration` |
| Function call callee (free fn) | `function` | — |
| Struct method call | `method` | — |
| Struct field access/decl | `property` | — |
| Struct type name (decl + use) | `struct`/`type` | `declaration` at decl |
| Enum type name | `enum`/`type` | — |
| Enum member (`E::Variant`) | `enumMember` | — |
| Module name (`import x`, `x::`) | `namespace` | `declaration` at import |
| Type annotations (`: int`) | `type` | — |
| Extern function name | `function` | `declaration` + `defaultLibrary`? |
| Operators (optional) | `operator` | — |
| String/number literals | usually left to TextMate | — |

Gaps to close vs. today:
- **Parameters**: currently parameters aren't distinctly tokenized inside
  function bodies (they resolve as `variable`). Mark params as `parameter` using
  the resolved symbol's origin, if the resolver distinguishes them; otherwise
  leave as `variable` (acceptable).
- **Type annotations**: the visitor doesn't walk `TurfType` annotations
  (`StructField` types, param types, var-decl declared types) — add tokens for
  those spans so `: MyStruct` colors the type. Requires the parser to expose the
  annotation's source span (small addition, akin to doc 02 §B).
- **Enum member on RHS of `::`**: already handled via `ResolvedKind ==
  EnumVariant`. Verify constants/statics color as `enumMember`/`property`
  appropriately.

### Delta & range
- **`full/delta`**: cache the last token array per `(uri, resultId)`; on the next
  full request compute the LSP delta edit script (VS Code sends the previous
  `resultId`). Reduces payload on large files during editing. Straightforward
  once tokens are produced deterministically sorted (they already sort by
  line/col).
- **`range`**: compute tokens only for AST nodes intersecting the requested
  range — filter the visitor output by range. Good for very large files.

### Name-location correctness
Once compiler item B lands, replace every `findNameLoc()` call with the node's
`getNameLoc()`. Until then keep `findNameLoc()` but note it can mis-span in
adversarial cases (its own doc). Semantic-token mis-spans are cosmetic (wrong
color), never correctness bugs, so shipping M1 on `findNameLoc()` is fine.

## Edge cases
- Multibyte characters: token `col`/`length` must be converted to the negotiated
  encoding via `PositionMapper` before delta-encoding (today the raw byte length
  is used — wrong for non-ASCII tokens).
- Overlapping tokens: ensure the visitor never emits two tokens covering the same
  span (LSP requires non-overlapping, ordered). The final `std::sort` handles
  order; add a de-dup/no-overlap assertion.
- Tokens inside imported-module bodies must **not** be emitted for the current
  document (already guarded in `visitModuleImport`, which deliberately does not
  recurse into `getBody()` — keep that).
- Partial/invalid buffers: emit tokens for whatever parsed; missing color on a
  broken region is fine.

## Phased tasks
- **P1.a** Consolidate to one `turf.tmLanguage.json`; verify against showcase.
- **P1.b** Route token columns/lengths through `PositionMapper` (encoding fix).
- **P1.c** Add type-annotation and parameter tokens; verify legend completeness.
- **P1.d** Implement `full/delta` with `resultId` caching.
- **P2.e** Implement `range` tokens for large files.
- **P2.f** Swap `findNameLoc()` → `getNameLoc()` after doc 02 §B.

## Acceptance tests
- A struct instance `p.x` colors `x` as `property`, `p.dist()` colors `dist` as
  `method`, `io::print` colors `io` as `namespace` and `print` as function.
- `immut` variables carry the `readonly` modifier; `mut` do not.
- Enum access `Color::Red` colors `Color` as namespace/enum and `Red` as
  `enumMember`.
- A file with a multibyte char in a string keeps every later token's coloring
  aligned (encoding correctness).
- Editing one line yields a small `full/delta`, not a full re-send.
- Disabling the server still leaves the file reasonably colored (TextMate layer).
