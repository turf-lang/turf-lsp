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
- **P2.1** Add `range` to the hover result (via `PositionMapper`).
- **P2.2** Extend `hoverText` to enums, enum members, fields, params, type
  annotations, module names.
- **P2.3** Doc-comments: interim LSP source re-scan (option 2), then switch to
  compiler-captured `getDocComment()` (option 1) when available.
- **P2.4** Overload listing at bare function names.

## Acceptance tests
- Hover a local shows `immut count: int` (or `mut`), underlining `count`.
- Hover a stdlib call (`io::print`) shows its signature, `module io`, and its
  doc-comment.
- Hover a struct field shows `x: double` and "field of `Point`".
- Hover an enum member shows "variant of enum Color".
- Hover a type annotation `: Point` shows the struct summary.
- Hover an undeclared identifier returns no hover (no crash, no wrong info).
