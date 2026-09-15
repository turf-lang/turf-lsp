#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

#include "DocumentAnalysis.hpp"

class CompilerService {
public:
    CompilerService();
    ~CompilerService() = default;

    // Must only ever be called from the single worker thread that owns the
    // compiler's global mutable state (doc 01 P0.1) -- never from the I/O
    // thread, and never concurrently with another call to analyze().
    //
    // `isCancelled`, if provided, is checked at the two natural phase
    // boundaries the pipeline already has (after parsing, before
    // resolveAll(); after resolveAll(), before checkAll()) -- coarse
    // checkpoint granularity, not mid-pass interruption, since the
    // compiler's passes themselves have no interruption points and adding
    // them would be a much larger change than an LSP's responsiveness
    // needs (doc 01 P0.1's cancellation section). Returns nullptr if
    // cancelled before completion.
    std::unique_ptr<DocumentAnalysis> analyze(std::string_view source, std::string_view filename,
                                               const std::function<bool()>& isCancelled = {});
};
