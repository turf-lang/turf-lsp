#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

class CompilerService {
public:
    CompilerService();
    ~CompilerService() = default;

    std::vector<nlohmann::json> analyze(std::string_view source, std::string_view filename);
};
