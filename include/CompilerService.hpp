#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

#include "DocumentAnalysis.hpp"

class CompilerService {
public:
    CompilerService();
    ~CompilerService() = default;

    std::unique_ptr<DocumentAnalysis> analyze(std::string_view source, std::string_view filename);
};
