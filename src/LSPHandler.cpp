#include "LSPHandler.hpp"
#include "SemanticQuery.hpp"
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
        } else if (method == "textDocument/completion") {
            handleCompletion(msg);
        } else if (method == "textDocument/didOpen") {
            handleDidOpen(msg);
        } else if (method == "textDocument/didChange") {
            handleDidChange(msg);
        } else if (method == "textDocument/didClose") {
            handleDidClose(msg);
        } else if (method == "textDocument/hover") {
            handleHover(msg);
        } else if (method == "textDocument/definition") {
            handleDefinition(msg);
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
            {"hoverProvider", true},
            {"definitionProvider", true},
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

void LSPHandler::handleCompletion(const json& request) {
    auto id = request.value("id", json(nullptr));

    // Basic keyword and builtin completion for Turf
    // A proper implementation would check the document context using the URI and position.
    json result = {
        {"isIncomplete", false},
        {"items", {
            // Keywords (Kind 14)
            {{"label", "fn"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "mut"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "immut"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "struct"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "extern"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "from"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "if"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "elseif"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "else"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "while"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "for"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "in"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "step"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "give"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "otherwise"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "break"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "continue"}, {"kind", 14}, {"detail", "Keyword"}},
            {{"label", "return"}, {"kind", 14}, {"detail", "Keyword"}},
            
            // Types (Kind 7)
            {{"label", "int"}, {"kind", 7}, {"detail", "Type"}},
            {{"label", "double"}, {"kind", 7}, {"detail", "Type"}},
            {{"label", "bool"}, {"kind", 7}, {"detail", "Type"}},
            {{"label", "string"}, {"kind", 7}, {"detail", "Type"}},
            {{"label", "ptr"}, {"kind", 7}, {"detail", "Type"}},
            {{"label", "void"}, {"kind", 7}, {"detail", "Type"}},
            
            // Builtins & Snippets (Kind 3 for Function/Method)
            {{"label", "assert"}, {"kind", 3}, {"detail", "Builtin Function"}, {"insertText", "assert(${1:condition}, \"${2:message}\")"}, {"insertTextFormat", 2}},
            {{"label", "io.printline"}, {"kind", 3}, {"detail", "Standard Library"}, {"insertText", "io.printline(${1:text})"}, {"insertTextFormat", 2}}
        }}
    };

    json response = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };

    sendMessage(response);
}

void LSPHandler::handleDidOpen(const json& notification) {
    auto params = notification.value("params", json::object());
    auto textDocument = params.value("textDocument", json::object());
    std::string uri = textDocument.value("uri", "");
    std::string text = textDocument.value("text", "");
    int version = textDocument.value("version", 0);

    m_documents[uri] = {text, version, nullptr};
    publishDiagnostics(uri);
}

void LSPHandler::handleDidChange(const json& notification) {
    auto params = notification.value("params", json::object());
    auto textDocument = params.value("textDocument", json::object());
    std::string uri = textDocument.value("uri", "");
    int version = textDocument.value("version", 0);

    auto contentChanges = params.value("contentChanges", json::array());
    if (!contentChanges.empty()) {
        std::string text = contentChanges[0].value("text", "");
        m_documents[uri] = {text, version, nullptr};
        publishDiagnostics(uri);
    }
}

void LSPHandler::handleDidClose(const json& notification) {
    auto params = notification.value("params", json::object());
    auto textDocument = params.value("textDocument", json::object());
    std::string uri = textDocument.value("uri", "");

    m_documents.erase(uri);

    json response = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/publishDiagnostics"},
        {"params", {
            {"uri", uri},
            {"diagnostics", json::array()}
        }}
    };
    sendMessage(response);
}

void LSPHandler::publishDiagnostics(const std::string& uri) {
    auto it = m_documents.find(uri);
    if (it == m_documents.end()) return;

    std::string path = uri;
    if (path.find("file://") == 0) {
        path = path.substr(7);
    }

    auto analysis = m_compiler.analyze(it->second.text, path);

    json response = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/publishDiagnostics"},
        {"params", {
            {"uri", uri},
            {"diagnostics", analysis->diagnostics}
        }}
    };
    sendMessage(response);

    it->second.analysis = std::move(analysis);
}

void LSPHandler::handleHover(const json& request) {
    auto id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    auto textDocument = params.value("textDocument", json::object());
    std::string uri = textDocument.value("uri", "");
    auto position = params.value("position", json::object());
    int line = position.value("line", 0) + 1; // LSP is 0-indexed, AST is 1-indexed
    int col = position.value("character", 0) + 1;

    json result = nullptr;
    auto it = m_documents.find(uri);
    if (it != m_documents.end() && it->second.analysis) {
        const ExprAST* node = SemanticQuery::findNodeAt(*(it->second.analysis), line, col);
        if (node) {
            auto text = SemanticQuery::hoverText(node);
            if (text) {
                result = {
                    {"contents", {
                        {"kind", "markdown"},
                        {"value", *text}
                    }}
                };
            }
        }
    }

    json response = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };
    sendMessage(response);
}

void LSPHandler::handleDefinition(const json& request) {
    auto id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    auto textDocument = params.value("textDocument", json::object());
    std::string uri = textDocument.value("uri", "");
    auto position = params.value("position", json::object());
    int line = position.value("line", 0) + 1;
    int col = position.value("character", 0) + 1;

    json result = nullptr;
    auto it = m_documents.find(uri);
    if (it != m_documents.end() && it->second.analysis) {
        const ExprAST* node = SemanticQuery::findNodeAt(*(it->second.analysis), line, col);
        if (node) {
            auto declLoc = SemanticQuery::getDeclarationLoc(node);
            if (declLoc && declLoc->Line > 0) { // Valid location
                // Assume same file for now (Phase 4 MVP)
                result = {
                    {"uri", uri},
                    {"range", {
                        {"start", {{"line", declLoc->Line - 1}, {"character", declLoc->Col - 1}}},
                        {"end", {{"line", declLoc->Line - 1}, {"character", declLoc->Col}}}
                    }}
                };
            }
        }
    }

    json response = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };
    sendMessage(response);
}

void LSPHandler::sendMessage(const json& message) {
    std::string serialized = message.dump();
    std::cout << "Content-Length: " << serialized.length() << "\r\n\r\n" << serialized;
    std::cout.flush();
}
