#pragma once

#include <vector>
#include <memory>
#include <nlohmann/json.hpp>
#include "AST.h"
#include "Resolver.h"

class DocumentAnalysis {
public:
    std::vector<std::unique_ptr<ExprAST>> asts;
    std::vector<nlohmann::json> diagnostics;

    // Kept alive past CompilerService::analyze() (instead of being destroyed
    // at the end of that call, as it was before) so completion/hover can
    // query resolver.symbolTable() -- module function/type lists, struct
    // registries, variable scopes -- after the fact. This is what makes
    // context-aware completion (io:: / instance.) possible at all.
    std::unique_ptr<Resolver> resolver;

    DocumentAnalysis() = default;
};
