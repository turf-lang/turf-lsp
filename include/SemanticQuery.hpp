#pragma once

#include "DocumentAnalysis.hpp"
#include "AST.h"
#include "ResolverSymbolTable.h"
#include <string>

class SemanticQuery {
public:
    // Finds the innermost AST node that intersects with the given Line and Col
    // Both Line and Col are 1-indexed.
    static const ExprAST* findNodeAt(const DocumentAnalysis& analysis, int line, int col);

    // Returns the location of the declaration for the symbol referenced by the node.
    static std::optional<ResolverSourceLocation> getDeclarationLoc(const ExprAST* node);

    // Formats a hover tooltip for the symbol referenced by the node.
    static std::optional<std::string> hoverText(const ExprAST* node);
};
