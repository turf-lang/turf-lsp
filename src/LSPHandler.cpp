#include "LSPHandler.hpp"
#include <iostream>
#include <string>

LSPHandler::LSPHandler() : m_shutdownRequested(false) {}

void LSPHandler::handleMessage(const std::string& message) {
    try {
        json msg = json::parse(message);
        
        // Ensure it's a valid JSON-RPC message
        if (!msg.contains("jsonrpc") || msg["jsonrpc"] != "2.0") {
            std::cerr << "Invalid JSON-RPC message received.\n";
            return;
        }

        std::string method = msg.value("method", "");

        if (method == "initialize") {
            handleInitialize(msg);
        } else if (method == "initialized") {
            handleInitialized(msg);
        } else if (method == "shutdown") {
            handleShutdown(msg);
        } else if (method == "exit") {
            handleExit();
        } else {
            // Handle other methods or ignore
            std::cerr << "Unhandled method: " << method << "\n";
        }
    } catch (const json::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << "\n";
    }
}

void LSPHandler::handleInitialize(const json& request) {
    // Read the request ID to send back the response
    auto id = request.value("id", json(nullptr));

    // Define the server capabilities
    json result = {
        {"capabilities", {
            {"textDocumentSync", 1}, // 1 = Full text document sync
            {"hoverProvider", false}, // Set to true when implemented
            {"completionProvider", {
                {"resolveProvider", false},
                {"triggerCharacters", {".", ":"}}
            }}
            // Add more capabilities here as they are implemented
        }},
        {"serverInfo", {
            {"name", "turf-lsp"},
            {"version", "0.1.0"}
        }}
    };

    json response = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };

    sendMessage(response);
}

void LSPHandler::handleInitialized(const json& notification) {
    // Client is ready. We can send diagnostics or show messages here.
    std::cerr << "turf-lsp initialized successfully.\n";
}

void LSPHandler::handleShutdown(const json& request) {
    m_shutdownRequested = true;
    auto id = request.value("id", json(nullptr));

    json response = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", nullptr}
    };

    sendMessage(response);
}

void LSPHandler::handleExit() {
    if (m_shutdownRequested) {
        std::exit(0);
    } else {
        std::exit(1);
    }
}

void LSPHandler::sendMessage(const json& message) {
    std::string serialized = message.dump();
    std::cout << "Content-Length: " << serialized.length() << "\r\n\r\n" << serialized;
    std::cout.flush();
}
