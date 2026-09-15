# 07 — Go to definition / declaration / type definition

## Goal
Jump from any use of a symbol to where it's defined, including across files into
imported user modules and the standard library.

## LSP surface
- `textDocument/definition` (have, partial + buggy).
- `textDocument/declaration` (add) — for Turf, declaration ≈ definition in most
  cases; `extern` decls are a genuine declaration-vs-definition split.
- `textDocument/typeDefinition` (add) — from a variable/expression to the
  definition of its *type* (struct/enum).
- All may return a `Location` or `LocationLink[]` (prefer `LocationLink` to give
  the client the source `originSelectionRange`).

## Current state
`handleDefinition` + `SemanticQuery::getDeclarationLoc`:
- Local variables, function calls, func defs, method calls resolve via
  `getResolved*()->DeclLoc` (+ `SourceModule`). Works for same-file targets.
- **Cross-file bug**: when `SourceModule` is non-empty it builds
  `CompilerExecutablePath/../lib/std/<module>.tr`. This is **wrong** now that
  stdlib modules live in subfolders (`lib/std/io/net.tr`,
  `lib/std/collections/list.tr`, ...). It also ignores user modules and
  `TURF_HOME`. Must be replaced with the compiler's real module resolution.

## Compiler data consumed
- Resolved-symbol `DeclLoc` (a `ResolverSourceLocation`) + `SourceModule` on
  functions/methods; `VariableSymbol::DeclLoc` for locals.
- Module→file resolution via `turf::query::resolveModuleFile()` (doc 02 §A),
  which mirrors `IndexModuleDir()`/`TURF_HOME`. **This is the fix.**
- For `typeDefinition`: the expression's resolved `TurfTypePtr` → its type name →
  the `StructDeclExprAST`/enum decl location (in the current file via the AST
  index, or in a module via `resolveModuleFile` + a lookup in that module's
  resolved symbols/registry).

## Design

### Definition
1. `findNodeAt(cursor)` → node.
2. `getDeclarationLoc(node)` → `{ ResolverSourceLocation loc, string moduleName }`.
3. Resolve target URI:
   - `moduleName` empty → same document URI.
   - else → `resolveModuleFile(moduleName, importingFile=thisDocPath, turfHome)`;
     wrap as `file://`. **Delete the hardcoded `lib/std/<name>.tr` path.**
4. Convert `loc` (+ name length via `NameLoc`/`findNameLoc`) to an LSP range via a
   `PositionMapper` **built for the target file** (read the target file's lines;
   don't reuse the source doc's mapper). Return `LocationLink` with
   `targetSelectionRange` = the name, `targetRange` = the whole decl if cheaply
   known (else same as selection).

### Declaration vs Definition
- For most symbols, `declaration` returns the same as `definition`.
- For `extern` functions: `declaration` → the `extern` decl; `definition` → the
  external/FFI origin is not in Turf source, so `definition` returns the same
  decl (or nothing) — document the behavior. `ExternFuncDeclExprAST` carries the
  decl location.
- For a struct **method call**, `declaration` → the method's `fn` in the struct;
  same as definition.

### Type definition
1. Node → resolved type (`getResolvedSymbol()->Type` for variables;
   `ReturnType` for a call expression; field type for a field access).
2. Type name → decl location:
   - Current file: look up in the AST index (`StructDecls`/enum names, like
     `DocumentIndexVisitor`).
   - Imported: `getTypeOwningModule(typeName)` → `resolveModuleFile` → locate the
     decl in that module (the module's resolved registry has the `DeclLoc`;
     expose it via the symbol table if not already — `registerStruct`/
     `registerEnumType` store a `Loc`).
3. Return the type decl's `LocationLink`.

### Cross-file target ranges
Targets in other files need that file's text to compute UTF-16 ranges. Cache a
lightweight `PositionMapper` per opened target file (the `WorkspaceIndex` from
doc 01 §7 can own these). For stdlib files the user hasn't opened, read on
demand.

## Edge cases
- Symbol resolves but `DeclLoc` is `{0,0}` (synthetic/builtin like `assert`,
  primitive methods) → return no location (can't navigate into a builtin).
  Optionally surface a hover-style note instead.
- Overloaded call: navigate to the *selected* overload's `DeclLoc`
  (`getResolvedCallTarget` already picks one).
- `Module::func` where the callee is a `ScopeAccessExprAST`: the resolver sets
  the target on the `FuncCallExprAST` (the visitor already special-cases this —
  keep it).
- Enum member `E::V` → definition = the variant's position in the enum decl (if
  tracked) else the enum decl itself.
- Cursor between two tokens / on punctuation → `findNodeAt` returns nothing →
  empty result.
- Target file moved/renamed since analysis → resolve fresh; if the file is gone,
  return empty and log.
- Multibyte in target file → target mapper handles it.

## Phased tasks
- ✅ **P2.1** Done (prior session): the stdlib path now goes through
  `resolveModuleFile()` (doc 02 §A) — fixes cross-file def for all
  relocated stdlib modules + user modules.
- ✅ **P2.2** Done (2026-09-15): returns a one-element `LocationLink[]`
  when the client's `initialize` capabilities declared
  `textDocument.definition.linkSupport`, else the original plain
  `Location` (unchanged wire shape for older clients). Carries
  `originSelectionRange` (reusing hover()'s per-node-kind span) plus
  `targetRange`/`targetSelectionRange` (same value -- no cheaply-known
  separate whole-decl range exists, so this is the documented "else"
  case, not a shortcut).
- ✅ **P2.3** Done (2026-09-15): `textDocument/declaration` reuses
  `doDefinition()` verbatim. Correct for every symbol kind resolved
  today; no extern-specific special case exists yet (nothing to special-
  case against, since `declaration`/`definition` are identical for every
  kind the LSP currently resolves).
- ✅ **P2.4** Done (2026-09-15): `SemanticQuery::getTypeDefinitionLoc()` +
  a new `TypeDeclIndexVisitor` (walks the current file's struct/enum
  decls plus every transitively-imported module's, via each
  `ModuleImportExprAST::getBody()` — no re-analysis of the target file
  needed, contrary to this doc's own speculation above). Verified live
  cross-file (a `net::TcpConnection`-typed variable correctly opens
  `lib/std/io/net.tr` at the real struct decl).
- ✅ **P2.5** Done (2026-09-15): `SymbolLocation` now carries the real
  target-name `length` (was a hardcoded 1-char caret), used for both the
  plain-`Location` and `LocationLink` response shapes, same-file and
  cross-file.

## Acceptance tests
- ✅ Go-to-def on `io::print` opens `lib/std/io/io.tr` (subfolder!) at the
  function — covered by the prior session's `ResolveModuleFile()` fix.
- ✅ Go-to-def on a `list`/`net` method or type opens the right subfolder
  file — same fix; typeDefinition additionally verified on `net::TcpConnection`.
- ✅ Go-to-def on a user `import`ed local module opens that file (`ResolveModuleFile()`
  checks the importing file's own directory).
- ✅ Go-to-def on a local variable jumps within the file to its `mut/immut` decl
  — verified live, byte-identical between `declaration` and `definition`.
- ✅ `typeDefinition` on a `Point` variable opens the `struct Point` decl —
  verified live, same-file case.
- ⬜ Go-to-def on `assert` (builtin) returns nothing gracefully — not
  re-verified this session (pre-existing `DeclLoc.Line > 0` guard should
  already cover it; not re-tested).
- Def target in a file containing multibyte chars lands on the right column.
