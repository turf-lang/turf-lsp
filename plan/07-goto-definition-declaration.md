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
- **P2.1** Replace the stdlib path with `resolveModuleFile()` (doc 02 §A). This
  alone fixes cross-file def for all relocated stdlib modules + user modules.
- **P2.2** Return `LocationLink` with proper selection/target ranges via a
  target-file `PositionMapper`.
- **P2.3** Implement `declaration` (mostly delegates to definition; special-case
  `extern`).
- **P2.4** Implement `typeDefinition` (variable/expr → its struct/enum decl).
- **P2.5** Use `NameLoc` (doc 02 §B) for precise target selection ranges.

## Acceptance tests
- Go-to-def on `io::print` opens `lib/std/io/io.tr` (subfolder!) at the function,
  proving the path bug is fixed.
- Go-to-def on a `list` method opens `lib/std/collections/list.tr`.
- Go-to-def on a user `import`ed local module opens that file.
- Go-to-def on a local variable jumps within the file to its `mut/immut` decl.
- `typeDefinition` on a `Point` variable opens the `struct Point` decl.
- Go-to-def on `assert` (builtin) returns nothing gracefully.
- Def target in a file containing multibyte chars lands on the right column.
