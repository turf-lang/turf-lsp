# 02 — Compiler-core API changes (turf-lang side)

> These are changes to **`turf-lang`**, not the LSP. Each is additive, and every
> one must keep `./scripts/test_runner.sh` green and follow `turf-lang/AGENTS.md`
> conventions (WHY-comments, one logical change per commit, a regression test for
> each fix). The LSP feature docs cross-reference the items here.

The guiding idea: the LSP currently reaches directly into compiler globals and
recovers missing data textually (e.g. `findNameLoc()` searching the source line
for a name the AST didn't record). That works but is fragile. This document
defines a **stable query surface** so the LSP depends on an intentional API, and
fixes the two data gaps (name locations, a references/occurrence index) that
force the textual hacks and block correct references/rename.

Order matters: **A and B unblock M0–M2 and should land in Phase 0.** C–F track
their consuming features.

---

## A. A stable `turf::lsp` query facade (Phase 0, required)

**Problem.** `CompilerService.cpp` depends on ~10 free globals and manually
save/restores `g_recoverJmp`, `g_recoverActive`, rebuilds `SourceLines`, etc.
Any refactor of the compiler's entry path silently breaks the LSP.

**Change.** Add `include/TurfQuery.h` / `src/TurfQuery.cpp` in `turf-lang`
exposing one entry point that encapsulates the reset/analyze/restore dance that
`analyze()` and `main.cpp`'s `runCompilerMain` both open-code today:

```cpp
namespace turf::query {
  struct AnalyzeOptions {
    std::string virtualPath;     // path used for import resolution
    bool collectAllDiagnostics;  // sets DiagnosticEngine::setCollectAllDiagnostics
    bool runTypeCheck = true;
    std::string turfHome;        // overrides TURF_HOME for module search
  };
  struct AnalyzeResult {
    std::vector<std::unique_ptr<ExprAST>> asts;
    std::unique_ptr<Resolver> resolver;
    std::vector<DiagnosticEngine::Diag> diagnostics; // raw, typed (Code/Category)
  };
  // Runs Lexer→Parser(recovery)→Resolver→TypeChecker on `source`. Encapsulates
  // ALL global reset/save/restore. Not reentrant (documented): the caller
  // (turf-lsp) serializes on one worker thread.
  AnalyzeResult analyze(std::string_view source, const AnalyzeOptions&);
}
```

The LSP's `CompilerService::analyze()` becomes a thin adapter over this. Benefit:
the global-state contract lives in one place in the compiler repo, tested by the
compiler's own suite, and the LSP stops duplicating it.

**✅ DONE (2026-09-15).** The module-file resolver half of this item is
implemented: `std::string ResolveModuleFile(const std::string &ModuleName,
const std::string &ImportingFile = "")` in `src/frontend/Parser.h` /
`src/frontend/ParserModule.cpp`, sharing `TurfInstallRoot()`/`IndexModuleDir()` with
`ParseModuleImportByName()` so it can never disagree with what a real `import`
actually resolves to (stdlib → usr package → importing-file directory → cwd,
pure lookup, no parsing/diagnostic side effects). This unblocks doc 07's bug
fix directly — no `turf::query` namespace wrapper was needed for this piece;
the plain free function follows the existing `IsPathUnderLibStd`-style
convention in `Parser.h`. Verified: `./scripts/test_runner.sh` 605/606 (the one
failure is a pre-existing flaky wall-clock sleep-timing test, unrelated,
reproduced in isolation).

The broader `turf::query::analyze()` facade (encapsulating the global
reset/save/restore dance) is **not yet done** — `CompilerService::analyze()`
still open-codes it. Remains valuable (single place to keep the global-state
contract correct) but is no longer a hard blocker for doc 07, since
`ResolveModuleFile` alone fixes the stdlib path bug.

**Regression test still to add:** a `ResolveModuleFile("net", ...)` unit/CLI
check asserting it finds `lib/std/io/net.tr` (the exact subfolder-relocation
case the old LSP hardcoding broke on).

---

## B. Record name-token locations in the AST — ✅ ALREADY DONE UPSTREAM

**Correction (2026-09-15):** this item was written assuming the gap described
below still existed. It does not — checking the actual headers during
implementation found the compiler already carries this data (tagged "issue
#230" throughout), most likely landed after the LSP's `findNameLoc()` workaround
was written and never back-ported into the LSP. **No turf-lang change needed
here; the remaining work is entirely on the turf-lsp side** (replace
`findNameLoc()` call sites with the real accessors — tracked as an LSP task,
see `turf-lsp/plan/04-code-highlighting.md` / `07-goto-definition-declaration.md`).

Confirmed present, each with a real `getNameLoc() -> const SourceLocation&`:
- `VarDeclExprAST::getNameLoc()` (`src/frontend/ASTDecl.h`)
- `FuncDefExprAST::getNameLoc()` (`src/frontend/ASTDecl.h`)
- `ExternFuncDeclExprAST::getNameLoc()` (`src/frontend/ASTDecl.h`)
- `StructDeclExprAST::getNameLoc()` (`src/frontend/ASTDecl.h`)
- `ModuleImportExprAST::getNameLoc()` (`src/frontend/ASTDecl.h`)
- `FieldAccessExprAST::getNameLoc()` (`src/frontend/ASTAccess.h`)
- `MethodCallExprAST::getNameLoc()` (`src/frontend/ASTAccess.h`)

That is the exact set of seven node kinds the LSP's `findNameLoc()` (in
`turf-lsp/src/SemanticQuery.cpp`) was built to recover textually. Original
problem statement, now moot for turf-lang, kept for context: those nodes used
to store only the *keyword*/operator's location, not the identifier's — e.g.
`struct E`'s `getLoc()` pointed at `s` in `struct`, not at `E`. The LSP's
`findNameLoc()` re-scanned the source line for the name text, which is
heuristic and boundary-fragile (its own comment names the "struct E vs Entity"
false-match risk this real API doesn't have).

**Sequencing:** the LSP can ship M1 on the existing textual recovery; land B
before find-references/rename (docs 08, 10), where correctness depends on it.

**Regression tests:** `--dump-ast`/a new `--dump-locs` mode (or a unit test)
asserting `NameLoc` points at the identifier for each node kind, including a
struct named `E` on a line that also contains `Entity`.

---

## C. A references / occurrence index from the Resolver (tracks doc 08)

**Problem.** The Resolver knows, per use-site, which declaration a name binds to
(it sets `getResolvedSymbol()` / `getResolvedCallTarget()` on nodes). But there's
no reverse map "declaration → all use sites," and no cross-file aggregation.

**Change (two options; prefer C1):**

- **C1 (preferred): stable symbol identity.** Give each resolved declaration a
  stable `SymbolId` (e.g. `SourceModule + "::" + mangledName` for functions,
  `file + DeclLoc` for locals). Expose on the resolved-symbol structs a
  `symbolId()`; expose on use-site nodes the `symbolId()` of their target. The
  LSP then builds the reverse index itself by walking each file's AST (single
  visitor pass) and bucketing occurrences by `symbolId`. No compiler-side global
  index needed; the LSP's `WorkspaceIndex` (doc 01 §7) owns aggregation.
- **C2 (heavier): compiler-side occurrence collection.** Have the Resolver emit a
  `std::vector<Occurrence{ SymbolId, SourceLocation, IsDeclaration }>` per
  analyzed file as a side output. Simpler for the LSP but bakes a
  tooling-specific concern into the compiler.

Recommend **C1**: minimal, keeps the compiler lean, and the LSP already walks
ASTs with visitors. It only needs *identity* to be stable and comparable across
files, which the mangled name (`SourceModule + BaseName + ParamTypes`, already
described in `ResolverSymbolBase.h`) mostly provides for functions/types;
locals need `file+DeclLoc`.

**Regression test (compiler side):** two nodes referring to the same declaration
report equal `symbolId()`; two same-named locals in different scopes report
different ids.

---

## D. A formatting engine (tracks doc 09; new subsystem)

**Problem.** No formatter exists anywhere in `turf-lang`.

**Change.** Build a formatter as a compiler subsystem (it needs the lexer's
token stream + the AST, both already available), exposed **two ways** so CLI and
LSP share one implementation:

1. CLI: `turf --format <file>` (writes to stdout) and `turf --format --write`
   (in place). Parallels existing `--dump-ast`, `--emit-llvm` flags in
   `src/main.cpp`'s `parseArgs`.
2. Library: `turf::format::format(std::string_view source, const FormatOptions&)
   -> FormatResult { std::string text; bool ok; }`, called in-process by the LSP.

Design details (rules, comment/trivia preservation, idempotence, partial-format)
live in doc 09; this item just fixes the *location* of the engine (in the
compiler, reused by the LSP, never a separate reimplementation).

**Regression tests:** golden-file tests under `turf-lang/tests/` — format each
`showcase/*.tr` and every relevant `tests/**/*.tr`, assert idempotence
(`format(format(x)) == format(x)`) and that formatted code still compiles
(reuses `test_runner.sh` semantics).

---

## E. Structured diagnostic export (tracks doc 03; small)

**Problem.** `DiagnosticEngine::Diag` already carries everything the LSP needs
(`Loc`, `Msg`, `Hint`, `IsWarning`, `RelatedLocations`, `Category`, `Filename`,
`Code`). The LSP currently reads the static vector directly. That's fine, but two
niceties:

- Ensure `Diag` also carries an **end location / length** for the primary span
  where the raising site knows it (many `TurfError`s know the offending token's
  length). Today the LSP fakes a 1-char-wide range (`Col` .. `Col+1`). A real end
  column makes squiggles cover the whole token. Where the raise site can't
  cheaply provide it, the LSP widens to the identifier under the start using the
  buffer (acceptable fallback).
- Keep the `--explain E0NN` machinery (`handleExplainFlagIfPresent` in
  `main.cpp`) — the LSP reuses the `docs/errors/codes/E0NN.txt` text for
  diagnostic `codeDescription` hrefs and hover-on-error (doc 03).

**Regression test:** an error with a known code still reports that code through
`turf::query::analyze`'s returned `diagnostics`.

### E2. Codes for syntax/lexer errors (optional, improves quick-fixes)

**Problem.** The `E001–E068` registry is overwhelmingly semantic/runtime. The
parser raises the large majority of **syntax** errors through the raw
`LogErrorAt(Loc, Msg)` path (~144 sites across `Parser*.cpp` vs ~24 typed
`raise()`), which produces a `Diag` with a location and message but **no `Code`,
no category, and a severity guessed from the message text**
(`isMessageWarning`). See doc 03 → "Coded vs. codeless diagnostics."

**Impact on the LSP.** None for *showing* diagnostics — codeless syntax errors
still squiggle correctly (LSP `code` is optional). The cost is that doc 10's
code-action quick-fixes lose their stable join key and must fall back to
**message-pattern matching** for syntax fixes (brittle, wording-coupled).

**Change (optional, staged).** Give the common syntax/lexer errors stable codes,
cheapest first:
- Add an overload `LogErrorAt(SourceLocation, const std::string& Msg, const
  std::string& Code)` that threads `Code` into `DiagnosticEngine::add`, and pass
  codes at the highest-value sites (`Expected ';'`, `Expected '}'`, `Expected
  ')'`, unexpected-token). New registry entries `E0NN` for these
  (category `SYNTAX_ERROR`), with matching `docs/errors/codes/E0NN.txt`.
- Longer term, migrate hot `LogErrorAt` sites to typed `TurfError` subclasses so
  they also gain hints/related-locations like the semantic errors already have.

**Payoff:** reliable syntax-error quick-fixes (doc 10), `--explain` links for
syntax errors, and message-independent regression tests. Purely additive; the LSP
forwards whatever `Code` is present with no change.

**Regression test:** a missing-semicolon parse error reports its new code through
`turf::query::analyze`.

---

## F. (Deferred, non-blocking) Per-analysis context object

**Problem/ambition.** The globals in doc 01 §2 cap the LSP at one analysis at a
time. The clean long-term fix is a `CompilerContext` struct threaded through the
pipeline (lexer cursor, symbol tables, diagnostic sink all become members), so
multiple analyses can run concurrently.

**Decision: out of scope for this plan.** It is a large, invasive refactor with
its own risk to the compiler's correctness, and the single-worker model (doc 01)
already meets a single-user editor's needs. Recorded here only so it isn't
re-discovered as "missing." Revisit only if profiling proves the single worker
is a bottleneck for real workspaces.

---

## Summary: what lands when

| Item | Compiler change | Blocks | Phase | Status |
|---|---|---|---|---|
| A. `resolveModuleFile` | small | doc 07 bug | 0 | ✅ done (`ResolveModuleFile` in `Parser.h`) |
| A. `turf::query::analyze` facade | small/med | (nice-to-have) | 0 | ⬜ not started |
| B. Name-token locations in AST | — | docs 04, 07, 08, 10 | 0/1 | ✅ already existed upstream (issue #230) |
| C. Stable `symbolId()` for references | small | doc 08, 10 | 3 | ⬜ not started |
| D. Formatting engine | new subsystem | doc 09 | 4 | ⬜ not started |
| E. Diagnostic end-locations + keep `--explain` | small | doc 03 (polish) | 1 | ⬜ not started |
| E2. Codes for syntax/lexer errors | small/staged | doc 10 quick-fixes (nicety) | 4 (optional) | ⬜ not started |
| F. Per-analysis context | large refactor | (nothing here) | — deferred | — deferred |
