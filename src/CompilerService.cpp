#include "CompilerService.hpp"
#include "Lexer.h"
#include "Parser.h"
#include "Resolver.h"
#include "TypeChecker.h"
#include "Errors.h"
#include "SymbolTable.h"
#include <iostream>
#include <sstream>
#include <cstring>

CompilerService::CompilerService() {
    // Required, one-time global setup the parser depends on -- main.cpp's own
    // entry point calls this before ever parsing anything. Without it,
    // BinopPrecedence (a plain static std::map<int,int>) stays empty, so
    // ParseBinOpRHS() treats every binary operator as equally (non-)
    // precedent and stops folding an expression's operator chain far too
    // early -- silently mis-parsing the first binary expression anywhere in
    // the compiled source (including inside every imported stdlib module)
    // and surfacing as a confusing, unrelated "Expected ';' after statement"
    // several tokens downstream of the real cause. The original
    // CompilerService never called this either; it's why imports of
    // anything beyond the most trivial module content silently failed.
    InitializePrecedence();
}

// Mirrors main.cpp's SkipToNextTopLevelBoundary() (static there, so not
// reusable directly) -- on a parse error, resync to the next top-level
// statement boundary instead of giving up on the rest of the file. Without
// this, a single syntax error anywhere aborts analysis for everything after
// it, which is unworkable for an LSP (the buffer is syntactically invalid on
// nearly every keystroke while the user is mid-edit).
static void skipToNextTopLevelBoundary() {
    do {
        if (CurTok == TOK_EOF) return;
        getNextToken();
    } while (g_braceDepth > 0);
}

std::unique_ptr<DocumentAnalysis> CompilerService::analyze(std::string_view source, std::string_view filename) {
    auto analysis = std::make_unique<DocumentAnalysis>();

    DiagnosticEngine::clearDiagnostics();
    ClearParsedTopLevelASTs();
    resetLexer();
    // Re-created fresh per analyze() call, not just once: struct/enum types
    // are registered into this at PARSE time (ParseStructDecl's
    // `GlobalSymbolTable->RegisterType(...)`, guarded on non-null -- it
    // silently no-ops if this is never initialized at all, which is exactly
    // what happened before this fix). TypeChecker's method-call resolution
    // (visitMethodCall) looks a base expression's struct type up THROUGH
    // this table, not just through ResolverSymbolTable -- without it, every
    // struct instance method call failed with a confusing "Cannot call
    // method '...' on type 'Point' -- only struct instances have methods.",
    // even though the real `turf` CLI compiles the identical source cleanly
    // (it always calls this during its own startup). Recreating it fresh
    // each call (rather than once in the constructor) avoids a renamed/
    // removed struct's old entry lingering forever across edits.
    InitializeSymbolTable();

    // Point the lexer directly at the buffer via SetSourceBuffer() (added
    // for exactly this -- "an LSP's unsaved editor buffer", see its doc
    // comment in Lexer.h / issue #232) instead of round-tripping through a
    // real temp file on disk next to the user's source, the previous
    // approach. gettok() only ever calls ->get()/->peek() on SourceFile, so
    // an in-memory std::istringstream works identically to a real ifstream.
    // sourceBuffer must outlive every SourceFile read below, so it's a local
    // in this call frame, not a temporary.
    std::istringstream sourceBuffer{std::string(source)};
    std::istream* oldFile = SourceFile;
    SetSourceBuffer(sourceBuffer);

    SourceLines.clear();
    {
        std::istringstream lineScanner{std::string(source)};
        std::string Line;
        while (std::getline(lineScanner, Line)) {
            SourceLines.push_back(Line);
        }
    }
    std::string oldSourceFilename = SourceFilename;
    // Import resolution only needs SourceFilename's PARENT DIRECTORY
    // (relative `import`s search it -- docs/architecture/
    // modules_and_scope_resolution.md §2.4); it no longer needs to exist on
    // disk at all now that content comes from SetSourceBuffer() above, not
    // from opening this path. The real filename can be used directly: no
    // temp-file naming/fallback dance, and diagnostics/hover attribute to
    // the real path with zero extra bookkeeping.
    SourceFilename = std::string(filename);
    AllSourceLines[SourceFilename] = SourceLines;

    // Save/restore the recovery jump buffer and flag around this whole call
    // so nested analyze() calls (there aren't any today, but this makes it
    // safe) or anything else touching global compiler state can't leave it
    // corrupted.
    jmp_buf savedJmpBuf;
    std::memcpy(savedJmpBuf, g_recoverJmp, sizeof(jmp_buf));
    bool savedRecoverActive = g_recoverActive;

    std::vector<std::unique_ptr<ExprAST>> ASTs;

    CurLoc = {1, 0};
    CurTok = 0;
    getNextToken();

    while (CurTok != TOK_EOF) {
        if (CurTok == ';') {
            getNextToken();
            continue;
        }

        g_recoverActive = true;
        std::unique_ptr<ExprAST> AST;
        bool parsedOk = false;
        if (setjmp(g_recoverJmp) == 0) {
            AST = ParseExpression();
            parsedOk = (AST != nullptr);
        }
        g_recoverActive = false;

        if (parsedOk) {
            if (CurTok == ';') {
                getNextToken();
            }
            ASTs.push_back(std::move(AST));
        } else {
            // Either ParseExpression() returned nullptr normally (it already
            // logged its own diagnostic), or raise() longjmp'd back here --
            // either way, resync to the next statement instead of losing the
            // rest of the document.
            skipToNextTopLevelBoundary();
        }
    }

    // Resolver/TypeChecker each still recover as a single unit (matching
    // main.cpp's own mandatory pass) -- an error deep in resolveAll()/
    // checkAll() still longjmps out of the whole call, but every node visited
    // before that point already has its resolved fields set, so a document
    // whose error is at or after the cursor still yields usable symbols for
    // everything before it.
    auto resolver = std::make_unique<Resolver>();
    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
        resolver->resolveAll(ASTs);
    }
    g_recoverActive = false;

    {
        TypeChecker TC(resolver->symbolTable());
        g_recoverActive = true;
        if (setjmp(g_recoverJmp) == 0) {
            TC.checkAll(ASTs);
        }
        g_recoverActive = false;
    }

    analysis->asts = std::move(ASTs);
    analysis->resolver = std::move(resolver);

    // Restore global state.
    g_recoverActive = savedRecoverActive;
    std::memcpy(g_recoverJmp, savedJmpBuf, sizeof(jmp_buf));
    SourceFile = oldFile;
    SourceFilename = oldSourceFilename;

    // Collect diagnostics for the caller to store (LSPHandler currently
    // chooses not to publish them to the client -- see publishDiagnostics()
    // -- but CompilerService itself still surfaces everything it saw).
    for (const auto& diag : DiagnosticEngine::getDiagnostics()) {
        nlohmann::json lspDiag = {
            {"range", {
                {"start", {{"line", diag.Loc.Line - 1}, {"character", diag.Loc.Col - 1}}},
                {"end", {{"line", diag.Loc.Line - 1}, {"character", diag.Loc.Col}}}
            }},
            {"severity", diag.IsWarning ? 2 : 1},
            {"message", diag.Msg}
        };
        analysis->diagnostics.push_back(lspDiag);
    }

    return analysis;
}
