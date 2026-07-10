#pragma once

#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

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

    // Helper to send a JSON-RPC response or notification
    void sendMessage(const json& message);

    bool m_shutdownRequested;
};
