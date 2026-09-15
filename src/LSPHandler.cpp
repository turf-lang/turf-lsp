#include "LSPHandler.hpp"
#include "SemanticQuery.hpp"
#include <iostream>
#include <string>
#include <cctype>
#include <llvm/Support/Path.h>
#include <llvm/ADT/SmallString.h>

extern std::string CompilerExecutablePath;

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
        } else if (method == "textDocument/semanticTokens/full") {
            handleSemanticTokens(msg);
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
            {"semanticTokensProvider", {
                {"legend", {
                    {"tokenTypes", {"variable", "function", "keyword", "type", "parameter", "struct", "string", "number", "operator", "namespace", "property", "enumMember", "method"}},
                    {"tokenModifiers", {"declaration", "readonly"}}
                }},
                {"full", true}
            }},
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

// Extracts the text of `line` (0-indexed) in `text`, up to (not including)
// `character` -- what SemanticQuery::getCompletions() classifies its trigger
// context from.
static std::string lineTextUpToCursor(const std::string& text, int line, int character) {
    size_t pos = 0;
    for (int i = 0; i < line && pos != std::string::npos; ++i) {
        pos = text.find('\n', pos);
        if (pos != std::string::npos) pos++;
    }
    if (pos == std::string::npos) return "";

    size_t lineEnd = text.find('\n', pos);
    if (lineEnd == std::string::npos) lineEnd = text.size();

    size_t cursorPos = pos + static_cast<size_t>(character);
    if (cursorPos > lineEnd) cursorPos = lineEnd;

    std::string line_ = text.substr(pos, cursorPos - pos);
    if (!line_.empty() && line_.back() == '\r') line_.pop_back();
    return line_;
}

static bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The buffer at the cursor is, almost by definition, mid-edit -- "io::" or
// "p." with nothing (or a partial word) after it is not valid syntax on its
// own, and turf-lang's recovery parser only resyncs at TOP-LEVEL statement
// boundaries (see the turf-lang GitHub issue filed on this), not within a
// function body. Left alone, a dangling trigger like this fails to parse the
// whole enclosing function, losing every local variable declared in it --
// exactly the declarations completion needs. Standard trick (also used by
// clangd et al.): replace whatever partial identifier is already typed right
// before the cursor with a synthetic, harmless placeholder identifier and a
// statement terminator, so the surrounding block parses as a normal (if
// semantically nonsensical) statement again. Used ONLY for a one-off,
// throwaway analyze() call for this completion request -- never stored back
// onto the document, which keeps showing the user's real, unmodified text.
static std::string patchForCompletion(const std::string& text, int line, int character) {
    size_t pos = 0;
    for (int i = 0; i < line && pos != std::string::npos; ++i) {
        pos = text.find('\n', pos);
        if (pos != std::string::npos) pos++;
    }
    if (pos == std::string::npos) return text;

    size_t lineEnd = text.find('\n', pos);
    if (lineEnd == std::string::npos) lineEnd = text.size();

    size_t cursorPos = pos + static_cast<size_t>(character);
    if (cursorPos > lineEnd) cursorPos = lineEnd;

    size_t prefixStart = cursorPos;
    while (prefixStart > pos && isIdentChar(text[prefixStart - 1])) prefixStart--;

    std::string patched = text;
    patched.replace(prefixStart, cursorPos - prefixStart, "turfLspCursor;");
    return patched;
}

void LSPHandler::handleCompletion(const json& request) {
    auto id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    auto textDocument = params.value("textDocument", json::object());
    std::string uri = textDocument.value("uri", "");
    auto position = params.value("position", json::object());
    int line = position.value("line", 0);
    int character = position.value("character", 0);

    json items = json::array();

    auto it = m_documents.find(uri);
    if (it != m_documents.end() && it->second.analysis) {
        std::string prefixText = lineTextUpToCursor(it->second.text, line, character);

        std::string path = uri;
        if (path.find("file://") == 0) path = path.substr(7);
        std::string patchedText = patchForCompletion(it->second.text, line, character);
        auto completionAnalysis = m_compiler.analyze(patchedText, path);

        auto completions = SemanticQuery::getCompletions(*completionAnalysis, prefixText);
        for (const auto& item : completions) {
            json lspItem = {
                {"label", item.label},
                {"kind", item.kind}
            };
            if (!item.detail.empty()) lspItem["detail"] = item.detail;
            if (!item.insertSnippet.empty()) {
                lspItem["insertText"] = item.insertSnippet;
                lspItem["insertTextFormat"] = 2; // Snippet
            }
            items.push_back(std::move(lspItem));
        }
    }

    json result = {
        {"isIncomplete", false},
        {"items", items}
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

    // Diagnostics are intentionally suppressed for now -- the analysis pass
    // above still runs (it's what populates the AST/symbol table completion
    // and hover depend on, and analysis->diagnostics is still collected), we
    // just don't forward them to the client yet. Re-enabling is a one-line
    // change (`analysis->diagnostics` instead of `json::array()`) once the
    // false-positive sources this was turned off for are fully sorted.
    json response = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/publishDiagnostics"},
        {"params", {
            {"uri", uri},
            {"diagnostics", json::array()}
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
            auto declLocOpt = SemanticQuery::getDeclarationLoc(node);
            if (declLocOpt && declLocOpt->loc.Line > 0) { // Valid location
                std::string targetUri = uri;

                // If it's a standard library module, resolve cross-file!
                if (!declLocOpt->moduleName.empty()) {
                    llvm::SmallString<128> StdlibPath(CompilerExecutablePath);
                    llvm::sys::path::remove_filename(StdlibPath);
                    llvm::sys::path::append(StdlibPath, "..", "lib", "std", declLocOpt->moduleName + ".tr");
                    targetUri = "file://" + StdlibPath.str().str();
                }

                result = {
                    {"uri", targetUri},
                    {"range", {
                        {"start", {{"line", declLocOpt->loc.Line - 1}, {"character", declLocOpt->loc.Col - 1}}},
                        {"end", {{"line", declLocOpt->loc.Line - 1}, {"character", declLocOpt->loc.Col}}}
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

void LSPHandler::handleSemanticTokens(const json& request) {
    auto id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    auto textDocument = params.value("textDocument", json::object());
    std::string uri = textDocument.value("uri", "");

    json result = nullptr;
    auto it = m_documents.find(uri);
    if (it != m_documents.end() && it->second.analysis) {
        auto tokens = SemanticQuery::getSemanticTokens(*(it->second.analysis));

        std::vector<int> data;
        int prevLine = 1;
        int prevCol = 1;

        for (const auto& token : tokens) {
            int deltaLine = token.line - prevLine;
            int deltaStartChar = (deltaLine == 0) ? (token.col - prevCol) : (token.col - 1);

            data.push_back(deltaLine);
            data.push_back(deltaStartChar);
            data.push_back(token.length);
            data.push_back(static_cast<int>(token.type));
            data.push_back(token.modifiers);

            prevLine = token.line;
            prevCol = token.col;
        }

        result = {
            {"data", data}
        };
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
