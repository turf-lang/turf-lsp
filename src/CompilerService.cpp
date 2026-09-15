#include "CompilerService.hpp"
#include "Lexer.h"
#include "Parser.h"
#include "Resolver.h"
#include "TypeChecker.h"
#include "Errors.h"
#include "SymbolTable.h"
#include <cctype>
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

// Many typed errors (see e.g. ErrorsScope.h's NameError) bake ANSI color
// escapes directly into Diag::Msg/Hint at construction time -- by design,
// for the CLI's own terminal output (SLMIntegration.cpp strips them right
// back out before handing error text to the SLM prompt, for the same
// reason: they're display-only formatting, not part of the message). An
// LSP diagnostic's `message` is plain text rendered by the editor's own UI
// -- passing raw escape codes through would show as garbage (or silently
// swallowed, client-dependent), never as color. Stripped here rather than
// at the source, matching how SLMIntegration.cpp already treats this as
// the consuming side's responsibility, not something to unwind at origin.
static std::string stripAnsiCodes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && s[j] != 'm') ++j;
            i = (j < s.size()) ? j + 1 : j;
        } else {
            out += s[i];
            ++i;
        }
    }
    return out;
}

// Widens a diagnostic's 1-byte point location to the span of the
// identifier/token it points at (doc 03 P1.3) -- a whole-token squiggle
// reads far better than a caret. The compiler doesn't carry real end-
// columns yet (SourceLocation::EndLine/EndCol stay at their -1 sentinel),
// so this is a best-effort scan through the already-captured source line,
// not a real lexer re-tokenization. Falls back to 1 byte whenever there's
// nothing identifier-shaped at the location (punctuation-only diagnostics,
// a hand-built {0,0} sentinel, or a filename/line AllSourceLines doesn't
// have -- e.g. a location that came from a file which failed to open).
static int widenDiagTokenByteLen(const std::string& filename, int line, int col) {
    auto it = AllSourceLines.find(filename);
    if (it == AllSourceLines.end()) return 1;
    const auto& lines = it->second;
    if (line < 1 || static_cast<size_t>(line) > lines.size()) return 1;
    const std::string& lineText = lines[static_cast<size_t>(line - 1)];
    if (col < 1 || static_cast<size_t>(col - 1) >= lineText.size()) return 1;

    size_t start = static_cast<size_t>(col - 1);
    auto isIdentChar = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
    if (!isIdentChar(static_cast<unsigned char>(lineText[start]))) return 1;

    size_t end = start;
    while (end < lineText.size() && isIdentChar(static_cast<unsigned char>(lineText[end]))) ++end;
    return static_cast<int>(end - start);
}

// Converts one compiler Diag into the LSP-shape-agnostic intermediate
// LSPHandler needs to finish the job (doc 03): resolves the diag's own
// (possibly-imported-module) filename, widens its point location, and
// folds Hint into the message text (LSP has no separate "hint" field).
static MappedDiagnostic mapDiagnostic(const DiagnosticEngine::Diag& diag, const std::string& analyzedFilename) {
    MappedDiagnostic md;
    md.loc.filename = !diag.Filename.empty() ? diag.Filename : analyzedFilename;
    md.loc.line = diag.Loc.Line;
    md.loc.col = diag.Loc.Col;
    md.loc.byteLen = widenDiagTokenByteLen(md.loc.filename, md.loc.line, md.loc.col);
    md.isWarning = diag.IsWarning;
    md.code = diag.Code;
    md.message = stripAnsiCodes(diag.Msg);
    if (!diag.Hint.empty()) md.message += "\nhint: " + stripAnsiCodes(diag.Hint);

    for (const auto& rel : diag.RelatedLocations) {
        MappedRelatedInfo mri;
        mri.loc.filename = !rel.Loc.Filename.empty() ? rel.Loc.Filename : md.loc.filename;
        mri.loc.line = rel.Loc.Line;
        mri.loc.col = rel.Loc.Col;
        mri.loc.byteLen = widenDiagTokenByteLen(mri.loc.filename, mri.loc.line, mri.loc.col);
        mri.description = stripAnsiCodes(rel.Description);
        md.related.push_back(std::move(mri));
    }
    return md;
}

std::unique_ptr<DocumentAnalysis> CompilerService::analyze(std::string_view source, std::string_view filename,
                                                             const std::function<bool()>& isCancelled) {
    auto analysis = std::make_unique<DocumentAnalysis>();

    DiagnosticEngine::clearDiagnostics();
    ClearParsedTopLevelASTs();
    resetLexer();

    // Never on for the LSP (doc 03 P1.1): `--smart` SLM suggestions are
    // slow and nondeterministic, wrong for a pass that reruns on every
    // debounced keystroke. Forced unconditionally rather than trusted to
    // already default false -- this is a documented contract of the LSP's
    // analysis pass, not an assumption about whatever main.cpp's own CLI
    // arg parsing happened to leave it at (which this binary never runs).
    g_AIEnabled = false;

    // add() caps at one primary error per source line by default (its own
    // doc comment) -- fine for a CLI's noise reduction, wrong for an LSP:
    // a second genuine error on an already-errored line must still surface
    // as its own squiggle (doc 03 P1.1). Saved/restored via restoreGlobals
    // below like every other piece of global state this function touches.
    bool savedCollectAllDiagnostics = DiagnosticEngine::getCollectAllDiagnostics();
    DiagnosticEngine::setCollectAllDiagnostics(true);
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

    // Restores every global this function touched. Must run on EVERY exit
    // path, including the two cancellation checkpoints below -- an early
    // `return nullptr` that skipped this would leave SourceFile/
    // SourceFilename/g_recoverJmp pointed at this call's (about-to-be-
    // destroyed) locals for whatever runs next on the worker thread.
    auto restoreGlobals = [&]() {
        g_recoverActive = savedRecoverActive;
        std::memcpy(g_recoverJmp, savedJmpBuf, sizeof(jmp_buf));
        SourceFile = oldFile;
        SourceFilename = oldSourceFilename;
        DiagnosticEngine::setCollectAllDiagnostics(savedCollectAllDiagnostics);
    };

    // Cancellation checkpoint (doc 01 P0.4): coarse-grained, between passes
    // rather than mid-pass -- see the rationale on the declaration in
    // CompilerService.hpp. Parsing itself is never cancelled mid-way: it's
    // typically the cheapest pass and a partial AST is less useful than
    // just letting it finish.
    if (isCancelled && isCancelled()) {
        restoreGlobals();
        return nullptr;
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

    if (isCancelled && isCancelled()) {
        restoreGlobals();
        return nullptr;
    }

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

    restoreGlobals();

    // Collect diagnostics for the caller (doc 03): mapDiagnostic() reads
    // AllSourceLines for token-widening, which restoreGlobals() above does
    // NOT clear (it's a persistent cross-pass cache, not per-call state --
    // see its own doc comment in Lexer.h), so this is safe to do after.
    for (const auto& diag : DiagnosticEngine::getDiagnostics()) {
        analysis->diagnostics.push_back(mapDiagnostic(diag, std::string(filename)));
    }

    return analysis;
}
