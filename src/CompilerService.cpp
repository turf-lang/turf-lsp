#include "CompilerService.hpp"
#include "Lexer.h"
#include "Parser.h"
#include "Resolver.h"
#include "TypeChecker.h"
#include "Errors.h"
#include "SymbolTable.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <chrono>
#include <llvm/Support/Path.h>
#include <llvm/ADT/SmallString.h>

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

    // Write the buffer to a temp file NEXT TO the real source file (not a
    // single shared /tmp path) so relative `import`s resolve correctly --
    // one of ParseImport()'s search paths is "the importing source file's
    // own directory" (docs/architecture/modules_and_scope_resolution.md
    // §2.4), which only works if the file we hand the parser actually lives
    // where the user's project thinks it does. Falls back to the system
    // temp dir if the real directory isn't writable (e.g. an unsaved/
    // untitled buffer) or has no directory component at all.
    std::string realPath(filename);
    llvm::SmallString<256> tempPathBuf;
    {
        llvm::StringRef dir = llvm::sys::path::parent_path(realPath);
        llvm::StringRef base = llvm::sys::path::filename(realPath);
        std::string baseName = base.empty() ? std::string("buffer.tr") : base.str();
        if (!dir.empty()) {
            tempPathBuf = dir;
        } else {
            llvm::sys::path::system_temp_directory(true, tempPathBuf);
        }
        llvm::sys::path::append(tempPathBuf, "." + baseName + ".turf-lsp-tmp");
    }
    std::string tempFile = tempPathBuf.str().str();
    {
        std::ofstream out(tempFile, std::ios::trunc);
        out << source;
        if (!out) {
            // Directory wasn't writable after all -- fall back to a system
            // temp path so analysis can still proceed (relative imports in
            // this document just won't resolve).
            llvm::SmallString<256> fallback;
            llvm::sys::path::system_temp_directory(true, fallback);
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            llvm::sys::path::append(fallback, "turf_lsp_" + std::to_string(stamp) + ".tr");
            tempFile = fallback.str().str();
            std::ofstream retry(tempFile, std::ios::trunc);
            retry << source;
        }
    }

    std::ifstream file(tempFile);
    std::ifstream* oldFile = SourceFile;
    SourceFile = &file;

    SourceLines.clear();
    {
        std::ifstream fileLines(tempFile);
        std::string Line;
        while (std::getline(fileLines, Line)) {
            SourceLines.push_back(Line);
        }
    }
    std::string oldSourceFilename = SourceFilename;
    // The real filename is what diagnostics/hover should attribute locations
    // to; the temp file only exists so the parser's own relative-import
    // search (which walks llvm::sys::path::parent_path(SourceFilename))
    // lands in the right directory. Both share the same parent directory by
    // construction above, so using the temp path here doesn't change import
    // resolution at all versus using the real path directly -- it just keeps
    // us from ever opening/writing the user's real file from the LSP.
    SourceFilename = tempFile;
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

    std::remove(tempFile.c_str());

    return analysis;
}
