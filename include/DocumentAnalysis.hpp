#pragma once

#include <string>
#include <vector>
#include <memory>
#include "AST.h"
#include "Resolver.h"

// A diagnostic location in the compiler's own coordinate space (1-indexed
// line/col, byte column) plus the real on-disk file it belongs to -- NOT
// necessarily the document that was analyzed, since an error can surface
// while resolving an `import`ed module (doc 03 "Multi-file"). `byteLen` is
// the widened span of the identifier/token under the point location (doc 03
// P1.3): the compiler doesn't track real end-columns yet (SourceLocation's
// EndLine/EndCol stay at their -1 sentinel), so this is a buffer-scan
// best-effort widen, falling back to 1 byte for anything that isn't
// alnum/underscore at that position (punctuation, EOF, a hand-built {0,0}
// sentinel location). LSPHandler is responsible for turning this into an
// LSP (line, character) range via PositionMapper -- CompilerService has no
// business owning position-encoding concerns.
struct MappedDiagLocation {
    std::string filename;
    int line = 0;
    int col = 0;
    int byteLen = 1;
};

struct MappedRelatedInfo {
    MappedDiagLocation loc;
    std::string description;
};

struct MappedDiagnostic {
    MappedDiagLocation loc;
    bool isWarning = false;
    std::string code;    // empty for the untyped LogErrorAt() raw path (doc 03)
    std::string message; // Diag.Msg, with "\nhint: ..." appended if Diag.Hint is non-empty
    std::vector<MappedRelatedInfo> related;
};

class DocumentAnalysis {
public:
    std::vector<std::unique_ptr<ExprAST>> asts;
    // The first `preludeImportCount` entries of `asts` are the 6 synthetic
    // `import int; import double; ...` nodes ParsePreludeImports() injects
    // ahead of the user's real top-level declarations (turf-lang auto-
    // imports these into every top-level file so e.g. bare string `+`
    // resolves without an explicit `import strings;` -- see
    // ParsePreludeImports()'s doc comment in turf-lang's Parser.h). They
    // MUST be included when resolving/typechecking (that's the entire
    // point -- without them, ordinary code using a prelude-provided
    // operator/UFCS method falsely fails to typecheck under the LSP even
    // though the real `turf` CLI compiles it fine), but every visitor that
    // walks `asts` for USER-facing output (semantic tokens, hover,
    // completion, findNodeAt) must skip them: they're synthesized with no
    // real second source position (NameLoc == Loc == wherever the real
    // file's first token happens to start -- see ModuleImportExprAST's own
    // NameLoc doc comment), so tokenizing/hit-testing them would produce up
    // to 6 phantom tokens overlapping the user's actual first line.
    size_t preludeImportCount = 0;
    std::vector<MappedDiagnostic> diagnostics;

    // Kept alive past CompilerService::analyze() (instead of being destroyed
    // at the end of that call, as it was before) so completion/hover can
    // query resolver.symbolTable() -- module function/type lists, struct
    // registries, variable scopes -- after the fact. This is what makes
    // context-aware completion (io:: / instance.) possible at all.
    std::unique_ptr<Resolver> resolver;

    DocumentAnalysis() = default;
};
