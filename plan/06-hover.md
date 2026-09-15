# 06 — Hover information

## Goal
Hovering any symbol shows a concise, typed summary: for a variable its
mutability + type; for a function its full signature (+ doc-comment); for a
struct its fields; for an enum its variants; for a module member its origin; for
a type annotation the type it names.

## LSP surface
`textDocument/hover` → `{ contents: MarkupContent(markdown), range }`.
`hoverProvider: true` (have). Include the `range` of the hovered token so the
client can highlight it (currently omitted).

## Current state
`SemanticQuery::hoverText(node)` handles:
- variable use / var decl → ``` `mut|immut name: Type` ```
- function call / func def / method call → ``` `fn <signature>` ```
- struct decl → struct block with fields.
Returned as markdown. `handleHover` finds the node via `findNodeAt` then formats.
No `range` returned; no doc-comments; no enums/params/fields/type-annotations;
no module-member hover.

## Compiler data consumed
- `VariableExprAST/VarDeclExprAST::getResolvedSymbol()` → `VariableSymbol{Name,
  Type, IsImmutable, DeclLoc}`.
- `FuncCallExprAST/FuncDefExprAST/MethodCallExprAST::getResolved*()` →
  `ResolverFunctionSymbol` with `signature()`, `Params`, `ReturnType`,
  `SourceModule`, `DeclLoc`.
- `StructDeclExprAST::getFields()`; enum types via
  `symbolTable().lookupEnumType()`; module membership via the module tables.
- Doc-comments: **need a source** — see below.

## Design

### Content per node kind (extend `hoverText`)
| Hover target | Rendered markdown |
|---|---|
| Variable / param | ```turf\n{mut|immut} name: Type\n``` + (if param) "parameter of `fn ...`" |
| Function (def or call) | ```turf\nfn signature\n``` + module origin line + doc-comment |
| Method call | same, prefixed with owning struct: `Struct.method` |
| Struct type name | struct block w/ fields (have) + method list summary |
| Struct field access | ```turf\nfield: Type\n``` + "field of `Struct`" |
| Enum type name | ```turf\nenum Name { A, B, C }\n``` |
| Enum member (`E::V`) | `V` — variant of `enum E` (+ its int value if known) |
| Module name (`import x`, `x::`) | "module `x`" + resolved file path |
| Type annotation (`: T`) | the type `T`'s definition summary |
| Literal | its type (`int`, `double`, `string`, `bool`) — optional |

### Range
Return the hovered token's `range` (via `PositionMapper`) so the client
underlines exactly the symbol. Use the node's `NameLoc` (doc 02 §B) or, until
then, the `findNameLoc()`-recovered span.

### Doc-comments (the main new capability)
The user's stdlib/showcase files are richly commented (see `showcase/*.tr`), and
`AGENTS.md` mandates WHY-comments. To show a function's doc on hover we need the
leading comment block above its declaration. Options:
1. **Lexer/parser captures doc-comments.** Cleanest: when the parser reads a
   declaration, attach the immediately-preceding contiguous `//` comment lines as
   a `DocComment` string on the decl node. This is a small compiler change
   (doc 02-style, additive) — the lexer already tracks comments to skip them; it
   just needs to remember the last run. Then hover reads `getDocComment()`.
2. **LSP re-scans the source** above `DeclLoc` for a contiguous `//` block.
   Zero compiler change; works for same-file decls; for cross-module decls needs
   to read the module file (already resolvable via doc 02 §A). Slightly hacky but
   fully in-LSP.

Recommend **(1)** for correctness and reuse (completion `resolve` and signature
help want the same doc string), with **(2)** as the interim implementation so
hover docs ship without waiting on the compiler change.

### Formatting
Use fenced ```turf code blocks for signatures (client applies the TextMate
grammar inside hovers) + plain markdown for prose (module origin, doc-comment).
Keep it tight — one code block + a couple of lines.

## Edge cases
- Unresolved node (typo, mid-edit) → return `null` (no hover) rather than a wrong
  guess. Optionally fall back to a purely lexical "keyword: ..." for keywords.
- **Known limitation, found 2026-09-15, not fixed this session**: none of
  the AST visitors (`NodeFinderVisitor`, `SemanticTokensVisitor`,
  `DocumentIndexVisitor`, `TypeDeclIndexVisitor`) override
  `visitRecoverExpr`, so anything textually inside a `recover ... produce
  ...` / `recover as err run { ... }` expression is invisible to
  hover/definition/typeDefinition/semantic-tokens/completion alike --
  confirmed live: `net::resolve_hostname("localhost") recover produce
  "FAIL"` produces zero semantic tokens and zero hover/definition results
  for the call inside the `recover`, while the identical call without
  `recover` works correctly. A real gap (error-handling is a first-class
  turf-lang feature, docs/planning/Errors.md), not attempted here --
  fixing it means adding a `visitRecoverExpr` override recursing into
  `RecoverExprAST`'s wrapped expression to (at minimum) all 4 visitors
  above, plus understanding whether the `produce`/`run` bodies need their
  own handling too. Left as a clearly-scoped follow-up.
- Overloaded functions: `getResolvedCallTarget()` gives the *selected* overload
  at a call; at a bare function *name* with multiple overloads, list all
  signatures.
- Hover on `self` → resolve to the enclosing struct type.
- Generic types (`List<int>`) → render the instantiated type name from
  `Type->getName()` (already includes generics per showcase).
- Hover inside imported module body isn't reachable from the current doc's AST
  (import bodies aren't hit-tested) — that's fine; hovering the *use* in the
  current file resolves to the target's signature via the resolved symbol.

## Phased tasks
- ✅ **P2.1** Done (2026-09-15): `range` added to every hover result.
- ✅ **P2.2** Done (2026-09-15): enum members, struct field access, and
  module names all hover correctly now. Params covered too (parameter
  hover appends "parameter"). Type-ANNOTATION hover (hovering the `: T`
  text itself, not a use of `T`) is not separately implemented -- hovering
  a type annotation lands on the same node a variable/field hover already
  answers via its declared type, so there's no separate "type annotation"
  hover target distinct from what P2.2's other cases already cover.
- ✅ **P2.3** Interim (LSP source re-scan, option 2) done (2026-09-15),
  same-file and cross-file. Compiler-captured `getDocComment()` (option
  1) not attempted -- still the documented "later correctness/reuse
  upgrade", not required for this to work today.
- ✅ **P2.4** Done (2026-09-16): hovering a bare function NAME with
  multiple overloads lists every signature (via
  `lookupCurrentFileFunctions()`, same lookup completion's
  `firstOverload()` already uses); hovering a CALL still shows only the
  selected overload, unchanged. Only current-file overloads are
  reachable this way -- an imported module's own decl is never
  hit-tested by `findNodeAt()`.

## Acceptance tests
- Hover a local shows `immut count: int` (or `mut`), underlining `count`.
- Hover a stdlib call (`io::print`) shows its signature, `module io`, and its
  doc-comment.
- Hover a struct field shows `x: double` and "field of `Point`".
- Hover an enum member shows "variant of enum Color".
- Hover a type annotation `: Point` shows the struct summary.
- Hover an undeclared identifier returns no hover (no crash, no wrong info).
