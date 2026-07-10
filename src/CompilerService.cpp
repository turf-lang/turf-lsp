#include "CompilerService.hpp"
#include "Lexer.h"
#include "Parser.h"
#include "Resolver.h"
#include "TypeChecker.h"
#include "Errors.h"
#include <iostream>

CompilerService::CompilerService() {}

std::unique_ptr<DocumentAnalysis> CompilerService::analyze(std::string_view source, std::string_view filename) {
    auto analysis = std::make_unique<DocumentAnalysis>();

    // Clear previous compiler state
    DiagnosticEngine::clearDiagnostics();
    ClearParsedTopLevelASTs();
    resetLexer();

    // Re-initialize lexer with string stream (we need to mock or change how SourceFile works)
    // Wait, Lexer.h uses `extern std::ifstream *SourceFile;`
    // If we want it to parse from memory, we need to adapt it. 
    // BUT for MVP, we can just dump the `source` to a temporary file and set SourceFile to that file.
    // Or, we can see if Lexer supports reading from a string. Let's dump to a temp file for now to avoid refactoring the lexer entirely.
    std::string tempFile = "/tmp/turflang_lsp_buffer.tr";
    {
        std::ofstream out(tempFile);
        out << source;
    }

    std::ifstream file(tempFile);
    std::ifstream* oldFile = SourceFile;
    SourceFile = &file;
    
    // Read lines for Lexer.h
    SourceLines.clear();
    std::ifstream fileLines(tempFile);
    std::string Line;
    while (std::getline(fileLines, Line)) {
        SourceLines.push_back(Line);
    }
    SourceFilename = std::string(filename);

    g_recoverActive = false; // We use exceptions

    try {
        CurLoc = {1, 0};
        CurTok = 0;
        getNextToken();

        std::vector<std::unique_ptr<ExprAST>> ASTs;

        while (CurTok != TOK_EOF) {
            if (CurTok == ';') {
                getNextToken();
                continue;
            }
            auto AST = ParseExpression();
            if (AST) {
                if (CurTok != ';') {
                    // eat ';'
                    getNextToken();
                } else {
                    getNextToken();
                }
                ASTs.push_back(std::move(AST));
            } else {
                getNextToken();
            }
        }

        Resolver R;
        R.resolveAll(ASTs);

        TypeChecker TC(R.symbolTable());
        TC.checkAll(ASTs);

        analysis->asts = std::move(ASTs);

    } catch (const TurfError::TurfException&) {
        // compilation aborted, errors are in DiagnosticEngine
    } catch (...) {
        // unexpected error
    }

    // Restore SourceFile
    SourceFile = oldFile;

    // Collect Diagnostics
    for (const auto& diag : DiagnosticEngine::getDiagnostics()) {
        nlohmann::json lspDiag = {
            // LSP lines and columns are 0-indexed
            {"range", {
                {"start", {{"line", diag.Loc.Line - 1}, {"character", diag.Loc.Col - 1}}},
                {"end", {{"line", diag.Loc.Line - 1}, {"character", diag.Loc.Col}}} 
            }},
            {"severity", diag.IsWarning ? 2 : 1}, // 1 = Error, 2 = Warning
            {"message", diag.Msg}
        };
        analysis->diagnostics.push_back(lspDiag);
    }

    return analysis;
}
