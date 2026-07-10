#pragma once

#include <vector>
#include <memory>
#include <nlohmann/json.hpp>
#include "AST.h"

class DocumentAnalysis {
public:
    std::vector<std::unique_ptr<ExprAST>> asts;
    std::vector<nlohmann::json> diagnostics;

    DocumentAnalysis() = default;
};
