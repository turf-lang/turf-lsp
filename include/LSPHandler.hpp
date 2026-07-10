#pragma once

#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "CompilerService.hpp"
#include "DocumentAnalysis.hpp"

using json = nlohmann::json;

struct DocumentState {
    std::string text;
    int version;
    std::unique_ptr<DocumentAnalysis> analysis;
};

class LSPHandler {
public:
    LSPHandler();
    ~LSPHandler() = default;

    // Parses a JSON-RPC message and dispatches it
    void handleMessage(const std::string& message);

private:
    // Core LSP Lifecycle methods
    void handleInitialize(const json& request);
    void handleInitialized(const json& notification);
    void handleShutdown(const json& request);
    void handleExit();
    void handleCompletion(const json& request);
    void handleDidOpen(const json& notification);
    void handleDidChange(const json& notification);
    void handleDidClose(const json& notification);
    void handleHover(const json& request);
    void handleDefinition(const json& request);
    void publishDiagnostics(const std::string& uri);

    // Helper to send a JSON-RPC response or notification
    void sendMessage(const json& message);

    std::unordered_map<std::string, DocumentState> m_documents;
    CompilerService m_compiler;
    bool m_shutdownRequested;
};
