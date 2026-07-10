#pragma once

#include "DocumentAnalysis.hpp"
#include "AST.h"
#include "ResolverSymbolTable.h"
#include <string>
#include <vector>

enum class SemanticTokenType {
    Variable = 0,
    Function = 1,
    Keyword = 2,
    Type = 3,
    Parameter = 4,
    Struct = 5,
    String = 6,
    Number = 7,
    Operator = 8
};

enum class SemanticTokenModifier {
    Declaration = 1 << 0,
    Readonly = 1 << 1
};

struct SemanticToken {
    int line;
    int col;
    int length;
    SemanticTokenType type;
    int modifiers;
};

struct SymbolLocation {
    ResolverSourceLocation loc;
    std::string moduleName;
};

class SemanticQuery {
public:
    // Finds the innermost AST node that intersects with the given Line and Col
    // Both Line and Col are 1-indexed.
    static const ExprAST* findNodeAt(const DocumentAnalysis& analysis, int line, int col);

    // Returns the location of the declaration for the symbol referenced by the node,
    // along with the module name if it's imported.
    static std::optional<SymbolLocation> getDeclarationLoc(const ExprAST* node);

    // Formats a hover tooltip for the symbol referenced by the node.
    static std::optional<std::string> hoverText(const ExprAST* node);

    // Extracts semantic tokens from the given document analysis.
    static std::vector<SemanticToken> getSemanticTokens(const DocumentAnalysis& analysis);
};
