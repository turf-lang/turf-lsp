#include "LSPHandler.hpp"
#include "SemanticQuery.hpp"
#include "Logger.hpp"
#include "Formatter.h"
#include "Parser.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace {

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Extracts the text of the line starting at byte offset `lineStart`, up to
// (not including) `cursorByteOffset` -- what SemanticQuery::getCompletions()
// classifies its trigger context from. Both offsets are absolute byte
// offsets into `text`; callers get them from a PositionMapper so this stays
// correct for non-ASCII text before the cursor (the LSP `character` a
// client sends is a UTF-16 code-unit count, not a byte count -- treating it
// as one directly, which this function used to do, is silently wrong for
// any line with non-ASCII content before the cursor).
std::string lineTextUpToByteOffset(const std::string& text, size_t lineStart, size_t cursorByteOffset) {
    size_t lineEnd = text.find('\n', lineStart);
    if (lineEnd == std::string::npos) lineEnd = text.size();
    size_t cursorPos = std::min(cursorByteOffset, lineEnd);
    std::string line = text.substr(lineStart, cursorPos - lineStart);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

// The buffer at the cursor is, almost by definition, mid-edit -- "io::" or
// "p." with nothing (or a partial word) after it is not valid syntax on its
// own, and turf-lang's recovery parser only resyncs at TOP-LEVEL statement
// boundaries, not within a function body. Left alone, a dangling trigger
// like this fails to parse the whole enclosing function, losing every
// local variable declared in it -- exactly the declarations completion
// needs. Standard trick (also used by clangd et al.): replace whatever
// partial identifier is already typed right before the cursor with a
// synthetic, harmless placeholder identifier and a statement terminator, so
// the surrounding block parses as a normal (if semantically nonsensical)
// statement again. Used ONLY for a one-off, throwaway analyze() call for
// this completion request -- never stored back onto the document, which
// keeps showing the user's real, unmodified text.
std::string patchForCompletionAt(const std::string& text, size_t lineStart, size_t cursorByteOffset) {
    size_t lineEnd = text.find('\n', lineStart);
    if (lineEnd == std::string::npos) lineEnd = text.size();
    size_t cursorPos = std::min(cursorByteOffset, lineEnd);

    size_t prefixStart = cursorPos;
    while (prefixStart > lineStart && isIdentChar(text[prefixStart - 1])) prefixStart--;

    std::string patched = text;
    patched.replace(prefixStart, cursorPos - prefixStart, "turfLspCursor;");
    return patched;
}

// Runs `fn` when it goes out of scope, regardless of how -- normal return
// or an exception unwinding through it. Used so a request's cancellation
// flag always gets unregistered exactly once, even on the rare path where
// a `do*` handler throws (workerLoop's own try/catch stops the exception
// from killing the process, but without this the cancel-flag map would
// leak that request's entry forever).
struct ScopeExit {
    std::function<void()> fn;
    ~ScopeExit() { if (fn) fn(); }
};

// Converts a compiler-native diagnostic location to an LSP range (doc 03
// P1.3/P1.4). `primaryMapper` is built from the document actually being
// analyzed's live buffer, so it's only valid for a location IN that same
// file (`loc.filename == primaryPath`) -- a location the compiler attributed
// to an imported module points at a file whose buffer isn't loaded here (it
// may not even be open in the editor), so it falls back to the same raw
// 1-indexed -> 0-indexed arithmetic doDefinition()'s cross-file branch uses,
// correct as long as the span itself is ASCII (true for every identifier
// turf-lang's lexer accepts; see that branch's own rationale comment).
LspRange rangeForDiagLocation(const MappedDiagLocation& loc, const std::string& primaryPath,
                               const PositionMapper& primaryMapper) {
    if (loc.filename == primaryPath) {
        return primaryMapper.toLspRange(loc.line, loc.col, loc.byteLen);
    }
    LspRange r;
    r.start = {loc.line - 1, loc.col - 1};
    r.end = {loc.line - 1, loc.col - 1 + std::max(loc.byteLen, 1)};
    return r;
}

// Splits `text` into lines (stripping a trailing '\r' on each, matching
// lineTextUpToByteOffset()'s own convention above), for readLeadingDocComment()
// below -- neither the current document's PositionMapper nor a target file
// read fresh off disk exposes a pre-split line vector directly.
std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

// Doc 06 P2.3's interim (LSP-side) doc-comment source: turf-lang's
// stdlib/showcase code is richly commented immediately above a
// declaration (AGENTS.md's WHY-comment convention), and hover is the one
// place a user actually wants to read that prose without opening the
// file. Scans backward from directly above `declLine` (1-indexed,
// compiler convention) for a CONTIGUOUS run of `//` lines -- stops at the
// first blank or non-`//` line, so a comment separated from the
// declaration by even one blank line is deliberately NOT picked up (it
// reads as unrelated to THIS declaration, not a stray formatting choice).
// Strips each line's `//` marker and one following space if present.
// Returns "" (not found) rather than guessing at a comment that isn't
// truly attached.
std::string readLeadingDocComment(const std::vector<std::string>& lines, int declLine) {
    std::vector<std::string> collected; // bottom-to-top as scanned
    int i = declLine - 2; // 0-indexed line directly above declLine
    while (i >= 0 && static_cast<size_t>(i) < lines.size()) {
        const std::string& line = lines[i];
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line.compare(start, 2, "//") != 0) break;
        std::string text = line.substr(start + 2);
        if (!text.empty() && text.front() == ' ') text.erase(0, 1);
        collected.push_back(std::move(text));
        --i;
    }
    if (collected.empty()) return "";
    std::string result;
    for (auto it = collected.rbegin(); it != collected.rend(); ++it) {
        if (!result.empty()) result += "\n";
        result += *it;
    }
    return result;
}

json jsonRange(const LspRange& r) {
    return {
        {"start", {{"line", r.start.line}, {"character", r.start.character}}},
        {"end", {{"line", r.end.line}, {"character", r.end.character}}}
    };
}

// Builds the response for definition/declaration/typeDefinition (doc 07
// P2.2/P2.5): a plain `Location` for clients that never declared
// `linkSupport` (unchanged wire shape from before this), or a one-element
// `LocationLink[]` when they did. `targetRange` is used for both
// `targetRange` and `targetSelectionRange` -- the whole-declaration range
// isn't cheaply known separately from the name's own span (see this doc's
// own note: "targetRange = the whole decl if cheaply known (else same as
// selection)"), so this is the "else" branch, not a shortcut taken instead
// of the real thing. `originSelectionRange` reuses SemanticQuery::hover()'s
// per-node-kind span computation purely for its .loc/.length -- the exact
// same "what's the source span of the thing under the cursor" question
// hover already answers for every node kind this handles, so recomputing
// it here via a second dynamic_cast chain would be pure duplication.
json buildDefinitionResult(const DocumentAnalysis& analysis, const std::string& targetUri,
                            const LspRange& targetRange, bool linkSupport, const ExprAST* node,
                            int compLine, int compCol, const PositionMapper& mapper) {
    if (!linkSupport) {
        return {
            {"uri", targetUri},
            {"range", jsonRange(targetRange)}
        };
    }
    json link = {
        {"targetUri", targetUri},
        {"targetRange", jsonRange(targetRange)},
        {"targetSelectionRange", jsonRange(targetRange)}
    };
    if (auto originInfo = SemanticQuery::hover(analysis, node, compLine, compCol)) {
        if (originInfo->length > 0) {
            link["originSelectionRange"] = jsonRange(mapper.toLspRange(originInfo->loc.Line, originInfo->loc.Col, originInfo->length));
        }
    }
    return json::array({link});
}

} // namespace

LSPHandler::LSPHandler() : m_debounceDelay(200) {
    Logger::init();
    if (const char* env = std::getenv("TURF_LSP_DEBOUNCE_MS")) {
        try {
            m_debounceDelay = std::chrono::milliseconds(std::stoi(env));
        } catch (const std::exception&) {
            Logger::warn(std::string("Ignoring malformed TURF_LSP_DEBOUNCE_MS='") + env + "'");
        }
    }
    m_workerThread = std::thread(&LSPHandler::workerLoop, this);
    m_debounceThread = std::thread(&LSPHandler::debounceLoop, this);
}

LSPHandler::~LSPHandler() {
    {
        std::lock_guard<std::mutex> lock(m_debounceMutex);
        m_stopDebounce = true;
    }
    m_debounceCv.notify_all();
    if (m_debounceThread.joinable()) m_debounceThread.join();

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stopWorker = true;
    }
    m_queueCv.notify_all();
    if (m_workerThread.joinable()) m_workerThread.join();
}

bool LSPHandler::isRequest(const json& msg) const {
    return msg.contains("id");
}

std::string LSPHandler::uriToPath(const std::string& uri) const {
    if (uri.rfind("file://", 0) == 0) return uri.substr(7);
    return uri;
}

std::string LSPHandler::pathToUri(const std::string& path) const {
    if (path.rfind("file://", 0) == 0) return path; // already a uri (defensive; callers pass real paths)
    return "file://" + path;
}

void LSPHandler::handleMessage(const std::string& message) {
    json msg;
    try {
        msg = json::parse(message);
    } catch (const json::exception& e) {
        Logger::warn(std::string("JSON parse error: ") + e.what());
        sendError(json(nullptr), -32700, std::string("Parse error: ") + e.what());
        return;
    }

    if (!msg.contains("jsonrpc") || msg["jsonrpc"] != "2.0") {
        Logger::warn("Invalid JSON-RPC message received (missing/bad jsonrpc field).");
        return;
    }

    // Guarantees the "exactly one response" contract even if a handler
    // throws (a thrown json::exception used to be swallowed silently here,
    // which left the client hanging on that request's id forever).
    try {
        dispatch(msg);
    } catch (const std::exception& e) {
        std::string method = msg.value("method", "");
        Logger::error("Unhandled exception dispatching '" + method + "': " + e.what());
        if (isRequest(msg)) {
            sendError(msg.value("id", json(nullptr)), -32603, std::string("Internal error: ") + e.what());
        }
    } catch (...) {
        std::string method = msg.value("method", "");
        Logger::error("Unhandled non-standard exception dispatching '" + method + "'");
        if (isRequest(msg)) {
            sendError(msg.value("id", json(nullptr)), -32603, "Internal error");
        }
    }
}

void LSPHandler::dispatch(const json& msg) {
    std::string method = msg.value("method", "");
    bool req = isRequest(msg);
    json id = req ? msg.value("id", json(nullptr)) : json(nullptr);

    // Protocol conformance (doc 01 P0.4): reject requests before
    // `initialize` / after `shutdown` rather than silently mishandling
    // them against not-yet-ready (or torn-down) server state; be lenient
    // for notifications since some clients send a few speculatively.
    if (m_shutdownRequested && method != "exit") {
        if (req) sendError(id, -32600, "Server has shut down");
        else Logger::warn("Ignoring notification after shutdown: " + method);
        return;
    }
    if (!m_initialized && method != "initialize" && method != "exit") {
        if (req) sendError(id, -32002, "Server not initialized");
        else Logger::warn("Ignoring notification before initialize: " + method);
        return;
    }

    if (method == "initialize") { handleInitialize(msg); return; }
    if (method == "initialized") { handleInitialized(msg); return; }
    if (method == "shutdown") { handleShutdown(msg); return; }
    if (method == "exit") { handleExit(); return; }
    if (method == "$/cancelRequest") { handleCancelRequest(msg); return; }
    if (method == "textDocument/didOpen") { handleDidOpen(msg); return; }
    if (method == "textDocument/didChange") { handleDidChange(msg); return; }
    if (method == "textDocument/didClose") { handleDidClose(msg); return; }
    if (method == "textDocument/completion") { handleCompletion(msg); return; }
    if (method == "completionItem/resolve") { handleCompletionResolve(msg); return; }
    if (method == "textDocument/hover") { handleHover(msg); return; }
    if (method == "textDocument/definition") { handleDefinition(msg); return; }
    if (method == "textDocument/declaration") { handleDeclaration(msg); return; }
    if (method == "textDocument/typeDefinition") { handleTypeDefinition(msg); return; }
    if (method == "textDocument/documentHighlight") { handleDocumentHighlight(msg); return; }
    if (method == "textDocument/references") { handleReferences(msg); return; }
    if (method == "textDocument/documentSymbol") { handleDocumentSymbol(msg); return; }
    if (method == "textDocument/formatting") { handleFormatting(msg); return; }
    if (method == "textDocument/semanticTokens/full") { handleSemanticTokens(msg); return; }
    if (method == "textDocument/semanticTokens/full/delta") { handleSemanticTokensDelta(msg); return; }

    if (req) {
        sendError(id, -32601, "Method not found: " + method);
    } else {
        Logger::debug("Unhandled notification: " + method);
    }
}

void LSPHandler::handleInitialize(const json& request) {
    auto id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());

    std::vector<std::string> clientEncodings;
    auto capabilities = params.value("capabilities", json::object());
    auto general = capabilities.value("general", json::object());
    if (general.contains("positionEncodings") && general["positionEncodings"].is_array()) {
        for (const auto& e : general["positionEncodings"]) {
            if (e.is_string()) clientEncodings.push_back(e.get<std::string>());
        }
    }
    m_positionEncoding = negotiatePositionEncoding(clientEncodings);

    auto textDocCaps = capabilities.value("textDocument", json::object());
    m_linkSupport = textDocCaps.value("definition", json::object()).value("linkSupport", false);

    std::vector<std::string> folders;
    if (params.contains("workspaceFolders") && params["workspaceFolders"].is_array()) {
        for (const auto& f : params["workspaceFolders"]) {
            folders.push_back(uriToPath(f.value("uri", "")));
        }
    } else if (params.contains("rootUri") && params["rootUri"].is_string()) {
        folders.push_back(uriToPath(params["rootUri"].get<std::string>()));
    }
    m_workspaceIndex.setWorkspaceFolders(folders);
    // Synchronous, not backgrounded: this only stats paths (never opens/
    // parses a file), so it's cheap even for a sizeable workspace, and
    // keeping it synchronous avoids a detached thread outliving `this` if
    // the client tears the server down again immediately after connecting.
    if (!folders.empty()) m_workspaceIndex.rescan();

    json result = {
        {"capabilities", {
            {"positionEncoding", positionEncodingWireName(m_positionEncoding)},
            {"textDocumentSync", {
                {"openClose", true},
                {"change", 2}, // incremental (doc 01 P0.5); range-less change events still fall back to full replace
                {"save", {{"includeText", false}}}
            }},
            {"hoverProvider", true},
            {"definitionProvider", true},
            {"declarationProvider", true},
            {"typeDefinitionProvider", true},
            {"documentHighlightProvider", true},
            {"referencesProvider", true},
            {"documentSymbolProvider", true},
            {"documentFormattingProvider", true},
            {"semanticTokensProvider", {
                {"legend", {
                    {"tokenTypes", {"variable", "function", "keyword", "type", "parameter", "struct", "string", "number", "operator", "namespace", "property", "enumMember", "method"}},
                    {"tokenModifiers", {"declaration", "readonly"}}
                }},
                {"full", {{"delta", true}}}
            }},
            {"completionProvider", {
                {"resolveProvider", true},
                {"triggerCharacters", {".", ":"}}
            }}
            // Add more capabilities here as they are implemented
        }},
        {"serverInfo", {
            {"name", "turf-lsp"},
            {"version", "0.1.0"}
        }}
    };

    sendResponse(id, result);
    // Set once the response is queued, not on the later `initialized`
    // notification -- a client is free to send didOpen etc. as soon as it
    // has our response, before it gets around to sending `initialized`.
    m_initialized = true;
}

void LSPHandler::handleInitialized(const json&) {
    Logger::info("turf-lsp initialized successfully.");
}

void LSPHandler::handleShutdown(const json& request) {
    m_shutdownRequested = true;
    sendResponse(request.value("id", json(nullptr)), nullptr);
}

void LSPHandler::handleExit() {
    std::exit(m_shutdownRequested ? 0 : 1);
}

void LSPHandler::handleCancelRequest(const json& notification) {
    auto params = notification.value("params", json::object());
    json id = params.value("id", json(nullptr));
    std::string key = id.dump();

    std::shared_ptr<std::atomic<bool>> flag;
    {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        auto it = m_cancelFlags.find(key);
        if (it != m_cancelFlags.end()) flag = it->second;
    }
    // If it's not in the map, it already finished (or was never
    // cancellable) -- nothing to do; per spec a late/unmatched
    // cancelRequest is simply ignored, not an error.
    if (flag) flag->store(true);
}

void LSPHandler::handleDidOpen(const json& notification) {
    auto params = notification.value("params", json::object());
    auto textDocument = params.value("textDocument", json::object());
    std::string uri = textDocument.value("uri", "");
    std::string text = textDocument.value("text", "");
    int64_t version = textDocument.value("version", 0);
    std::string languageId = textDocument.value("languageId", std::string("turf"));

    m_documents.open(uri, std::move(text), version, std::move(languageId));
    scheduleDebouncedDiagnostics(uri, version);
}

void LSPHandler::handleDidChange(const json& notification) {
    auto params = notification.value("params", json::object());
    auto textDocument = params.value("textDocument", json::object());
    std::string uri = textDocument.value("uri", "");
    int64_t version = textDocument.value("version", 0);

    auto contentChanges = params.value("contentChanges", json::array());
    for (const auto& change : contentChanges) {
        if (change.contains("range") && change["range"].is_object()) {
            auto rangeJson = change["range"];
            LspRange range;
            range.start = {rangeJson["start"].value("line", 0), rangeJson["start"].value("character", 0)};
            range.end = {rangeJson["end"].value("line", 0), rangeJson["end"].value("character", 0)};
            std::string newText = change.value("text", "");
            m_documents.applyRangedEdit(uri, range, newText, version, m_positionEncoding);
        } else {
            // No `range` -- per the LSP spec this event replaces the whole
            // document, the legal fallback even under `change: 2`
            // (incremental) if a client ever sends one this way.
            std::string text = change.value("text", "");
            m_documents.replaceText(uri, std::move(text), version);
        }
    }

    if (!contentChanges.empty()) {
        scheduleDebouncedDiagnostics(uri, version);
    }
}

void LSPHandler::handleDidClose(const json& notification) {
    auto params = notification.value("params", json::object());
    auto textDocument = params.value("textDocument", json::object());
    std::string uri = textDocument.value("uri", "");

    m_documents.close(uri);
    {
        std::lock_guard<std::mutex> lock(m_debounceMutex);
        m_pendingDebounce.erase(uri); // drop any debounce still in flight for the now-closed document
    }

    // Clear squiggles this document's own uri was carrying, plus any
    // imported-module uris its last analysis pass attributed diagnostics to
    // (doc 03 P1.4) -- best-effort: if another still-open document also
    // imports the same module and legitimately still has diagnostics to
    // report there, its own next publish will re-assert them. There is no
    // cross-primary reference counting here; this is a deliberate
    // simplification, not a correctness guarantee for that edge case.
    std::set<std::string> staleSecondary;
    {
        std::lock_guard<std::mutex> lock(m_diagMutex);
        auto it = m_secondaryDiagUris.find(uri);
        if (it != m_secondaryDiagUris.end()) {
            staleSecondary = std::move(it->second);
            m_secondaryDiagUris.erase(it);
        }
    }

    sendMessage({
        {"jsonrpc", "2.0"},
        {"method", "textDocument/publishDiagnostics"},
        {"params", {{"uri", uri}, {"diagnostics", json::array()}}}
    });
    for (const auto& secondaryUri : staleSecondary) {
        sendMessage({
            {"jsonrpc", "2.0"},
            {"method", "textDocument/publishDiagnostics"},
            {"params", {{"uri", secondaryUri}, {"diagnostics", json::array()}}}
        });
    }
}

void LSPHandler::handleCompletion(const json& request) {
    json id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    std::string uri = params.value("textDocument", json::object()).value("uri", "");
    auto position = params.value("position", json::object());
    LspPosition pos{position.value("line", 0), position.value("character", 0)};

    auto cancelFlag = registerCancellable(id);
    enqueueWork([this, id, uri, pos, cancelFlag]() { doCompletion(id, uri, pos, cancelFlag); });
}

void LSPHandler::handleCompletionResolve(const json& request) {
    json id = request.value("id", json(nullptr));
    // Unlike every other request here, `params` for completionItem/resolve
    // IS the CompletionItem itself (per spec), not a textDocument/position
    // wrapper -- passed straight through to the worker, which reads back
    // out of it exactly what doCompletion() put into its `data` field.
    json item = request.value("params", json::object());

    auto cancelFlag = registerCancellable(id);
    enqueueWork([this, id, item, cancelFlag]() { doCompletionResolve(id, item, cancelFlag); });
}

void LSPHandler::handleHover(const json& request) {
    json id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    std::string uri = params.value("textDocument", json::object()).value("uri", "");
    auto position = params.value("position", json::object());
    LspPosition pos{position.value("line", 0), position.value("character", 0)};

    auto cancelFlag = registerCancellable(id);
    enqueueWork([this, id, uri, pos, cancelFlag]() { doHover(id, uri, pos, cancelFlag); });
}

void LSPHandler::handleDefinition(const json& request) {
    json id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    std::string uri = params.value("textDocument", json::object()).value("uri", "");
    auto position = params.value("position", json::object());
    LspPosition pos{position.value("line", 0), position.value("character", 0)};

    auto cancelFlag = registerCancellable(id);
    enqueueWork([this, id, uri, pos, cancelFlag]() { doDefinition(id, uri, pos, cancelFlag); });
}

void LSPHandler::handleDeclaration(const json& request) {
    json id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    std::string uri = params.value("textDocument", json::object()).value("uri", "");
    auto position = params.value("position", json::object());
    LspPosition pos{position.value("line", 0), position.value("character", 0)};

    auto cancelFlag = registerCancellable(id);
    // Reuses doDefinition() directly -- see this method's doc comment in
    // LSPHandler.hpp for why that's correct for every symbol kind resolved
    // today.
    enqueueWork([this, id, uri, pos, cancelFlag]() { doDefinition(id, uri, pos, cancelFlag); });
}

void LSPHandler::handleTypeDefinition(const json& request) {
    json id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    std::string uri = params.value("textDocument", json::object()).value("uri", "");
    auto position = params.value("position", json::object());
    LspPosition pos{position.value("line", 0), position.value("character", 0)};

    auto cancelFlag = registerCancellable(id);
    enqueueWork([this, id, uri, pos, cancelFlag]() { doTypeDefinition(id, uri, pos, cancelFlag); });
}

void LSPHandler::handleDocumentHighlight(const json& request) {
    json id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    std::string uri = params.value("textDocument", json::object()).value("uri", "");
    auto position = params.value("position", json::object());
    LspPosition pos{position.value("line", 0), position.value("character", 0)};

    auto cancelFlag = registerCancellable(id);
    enqueueWork([this, id, uri, pos, cancelFlag]() { doDocumentHighlight(id, uri, pos, cancelFlag); });
}

void LSPHandler::handleReferences(const json& request) {
    json id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    std::string uri = params.value("textDocument", json::object()).value("uri", "");
    auto position = params.value("position", json::object());
    LspPosition pos{position.value("line", 0), position.value("character", 0)};
    bool includeDeclaration = params.value("context", json::object()).value("includeDeclaration", true);

    auto cancelFlag = registerCancellable(id);
    enqueueWork([this, id, uri, pos, includeDeclaration, cancelFlag]() {
        doReferences(id, uri, pos, includeDeclaration, cancelFlag);
    });
}

void LSPHandler::handleDocumentSymbol(const json& request) {
    json id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    std::string uri = params.value("textDocument", json::object()).value("uri", "");

    auto cancelFlag = registerCancellable(id);
    enqueueWork([this, id, uri, cancelFlag]() { doDocumentSymbol(id, uri, cancelFlag); });
}

void LSPHandler::handleFormatting(const json& request) {
    json id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    std::string uri = params.value("textDocument", json::object()).value("uri", "");
    auto formatOpts = params.value("options", json::object());
    bool insertSpaces = formatOpts.value("insertSpaces", true);
    int tabSize = formatOpts.value("tabSize", 4);

    auto cancelFlag = registerCancellable(id);
    enqueueWork([this, id, uri, insertSpaces, tabSize, cancelFlag]() {
        doFormatting(id, uri, insertSpaces, tabSize, cancelFlag);
    });
}

void LSPHandler::handleSemanticTokens(const json& request) {
    json id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    std::string uri = params.value("textDocument", json::object()).value("uri", "");

    auto cancelFlag = registerCancellable(id);
    enqueueWork([this, id, uri, cancelFlag]() { doSemanticTokens(id, uri, cancelFlag); });
}

void LSPHandler::handleSemanticTokensDelta(const json& request) {
    json id = request.value("id", json(nullptr));
    auto params = request.value("params", json::object());
    std::string uri = params.value("textDocument", json::object()).value("uri", "");
    std::string previousResultId = params.value("previousResultId", "");

    auto cancelFlag = registerCancellable(id);
    enqueueWork([this, id, uri, previousResultId, cancelFlag]() {
        doSemanticTokensDelta(id, uri, previousResultId, cancelFlag);
    });
}

std::shared_ptr<std::atomic<bool>> LSPHandler::registerCancellable(const json& id) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> lock(m_cancelMutex);
    m_cancelFlags[id.dump()] = flag;
    return flag;
}

void LSPHandler::unregisterCancellable(const std::string& idKey) {
    std::lock_guard<std::mutex> lock(m_cancelMutex);
    m_cancelFlags.erase(idKey);
}

void LSPHandler::enqueueWork(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push_back(std::move(task));
    }
    m_queueCv.notify_one();
}

void LSPHandler::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this] { return m_stopWorker || !m_queue.empty(); });
            if (m_stopWorker && m_queue.empty()) return;
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        try {
            task();
        } catch (const std::exception& e) {
            Logger::error(std::string("worker task threw: ") + e.what());
        } catch (...) {
            Logger::error("worker task threw a non-standard exception");
        }
    }
}

void LSPHandler::scheduleDebouncedDiagnostics(const std::string& uri, int64_t version) {
    {
        std::lock_guard<std::mutex> lock(m_debounceMutex);
        // Overwrites any earlier pending entry for this uri -- this IS the
        // coalescing: a burst of keystrokes just keeps pushing the due
        // time out, so only the last edit in a quiet-period window ever
        // gets analyzed/published (doc 01 P0.1).
        m_pendingDebounce[uri] = PendingDebounce{version, std::chrono::steady_clock::now() + m_debounceDelay};
    }
    m_debounceCv.notify_all();
}

void LSPHandler::debounceLoop() {
    std::unique_lock<std::mutex> lock(m_debounceMutex);
    while (!m_stopDebounce) {
        if (m_pendingDebounce.empty()) {
            m_debounceCv.wait(lock, [this] { return m_stopDebounce || !m_pendingDebounce.empty(); });
            continue;
        }

        auto earliest = m_pendingDebounce.begin();
        for (auto it = m_pendingDebounce.begin(); it != m_pendingDebounce.end(); ++it) {
            if (it->second.dueAt < earliest->second.dueAt) earliest = it;
        }

        auto now = std::chrono::steady_clock::now();
        if (earliest->second.dueAt > now) {
            m_debounceCv.wait_until(lock, earliest->second.dueAt);
            continue;
        }

        std::string uri = earliest->first;
        m_pendingDebounce.erase(earliest);
        lock.unlock();
        enqueueWork([this, uri]() { doPublishDiagnostics(uri); });
        lock.lock();
    }
}

std::shared_ptr<const DocumentAnalysis> LSPHandler::ensureFreshAnalysis(const std::string& uri,
                                                                          const std::function<bool()>& isCancelled) {
    auto textSnap = m_documents.getText(uri);
    if (!textSnap) return nullptr; // closed

    auto anSnap = m_documents.getAnalysis(uri);
    if (anSnap && anSnap->analysis && anSnap->analyzedVersion == textSnap->version) {
        return anSnap->analysis; // already fresh -- no need to reanalyze
    }

    std::string path = uriToPath(uri);
    std::unique_ptr<DocumentAnalysis> fresh = m_compiler.analyze(textSnap->text, path, isCancelled);
    if (!fresh) return nullptr; // cancelled mid-analysis

    std::shared_ptr<const DocumentAnalysis> shared = std::move(fresh);
    m_documents.setAnalysis(uri, textSnap->version, shared);
    return shared;
}

void LSPHandler::doPublishDiagnostics(std::string uri) {
    if (!m_documents.contains(uri)) return; // closed before the worker got to it

    auto analysis = ensureFreshAnalysis(uri);
    if (!analysis) return; // closed underneath us mid-analysis

    auto textSnap = m_documents.getText(uri);
    if (!textSnap) return;

    std::string primaryPath = uriToPath(uri);
    PositionMapper mapper(textSnap->text, m_positionEncoding);

    // Group by which real file each diagnostic belongs to (doc 03
    // "Multi-file"): most land on primaryPath, but one raised while
    // resolving an `import`ed module is attributed to THAT file's path and
    // must be published against its own uri, not the primary document's.
    std::map<std::string, std::vector<json>> byFile;
    for (const auto& md : analysis->diagnostics) {
        json d = {
            {"range", jsonRange(rangeForDiagLocation(md.loc, primaryPath, mapper))},
            {"severity", md.isWarning ? 2 : 1},
            {"source", "turf"},
            {"message", md.message}
        };
        if (!md.code.empty()) d["code"] = md.code;
        if (!md.related.empty()) {
            json relArr = json::array();
            for (const auto& rel : md.related) {
                relArr.push_back({
                    {"location", {
                        {"uri", pathToUri(rel.loc.filename)},
                        {"range", jsonRange(rangeForDiagLocation(rel.loc, primaryPath, mapper))}
                    }},
                    {"message", rel.description}
                });
            }
            d["relatedInformation"] = relArr;
        }
        byFile[md.loc.filename].push_back(std::move(d));
    }

    // Always publish for the primary document itself, even with zero
    // diagnostics -- this is what clears stale squiggles once the last
    // error on a line is fixed. `version` lets the client discard this
    // publish if a newer edit has already superseded it.
    json primaryArr = json::array();
    auto primaryIt = byFile.find(primaryPath);
    if (primaryIt != byFile.end()) {
        for (auto& d : primaryIt->second) primaryArr.push_back(std::move(d));
    }
    sendMessage({
        {"jsonrpc", "2.0"},
        {"method", "textDocument/publishDiagnostics"},
        {"params", {{"uri", uri}, {"version", textSnap->version}, {"diagnostics", primaryArr}}}
    });

    // Secondary (imported-module) files: publish each non-empty group, then
    // clear (empty-array publish) any uri that carried diagnostics on the
    // PREVIOUS publish for this primary document but not this one --
    // without this, a fixed import-side error would leave a stale squiggle
    // in a file that's no longer being told anything is wrong with it.
    std::set<std::string> newSecondary;
    for (auto& [filename, diags] : byFile) {
        if (filename == primaryPath) continue;
        std::string secondaryUri = pathToUri(filename);
        newSecondary.insert(secondaryUri);
        json arr = json::array();
        for (auto& d : diags) arr.push_back(std::move(d));
        sendMessage({
            {"jsonrpc", "2.0"},
            {"method", "textDocument/publishDiagnostics"},
            {"params", {{"uri", secondaryUri}, {"diagnostics", arr}}}
        });
    }

    std::set<std::string> staleSecondary;
    {
        std::lock_guard<std::mutex> lock(m_diagMutex);
        auto& prevSecondary = m_secondaryDiagUris[uri];
        for (const auto& prevUri : prevSecondary) {
            if (!newSecondary.count(prevUri)) staleSecondary.insert(prevUri);
        }
        prevSecondary = std::move(newSecondary);
    }
    for (const auto& staleUri : staleSecondary) {
        sendMessage({
            {"jsonrpc", "2.0"},
            {"method", "textDocument/publishDiagnostics"},
            {"params", {{"uri", staleUri}, {"diagnostics", json::array()}}}
        });
    }
}

void LSPHandler::doHover(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag) {
    std::string idKey = id.dump();
    ScopeExit cleanup{[this, idKey] { unregisterCancellable(idKey); }};

    auto cancelled = [&cancelFlag] { return cancelFlag && cancelFlag->load(); };
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!m_documents.contains(uri)) { sendError(id, -32801, "Content modified"); return; }

    auto analysis = ensureFreshAnalysis(uri, cancelled);
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!analysis) { sendResponse(id, nullptr); return; }

    auto textSnap = m_documents.getText(uri);
    if (!textSnap) { sendError(id, -32801, "Content modified"); return; }

    PositionMapper mapper(textSnap->text, m_positionEncoding);
    int compLine, compCol;
    mapper.toCompilerLineCol(pos, compLine, compCol);

    json result = nullptr;
    const ExprAST* node = SemanticQuery::findNodeAt(*analysis, compLine, compCol);
    if (node) {
        auto info = SemanticQuery::hover(*analysis, node, compLine, compCol);
        if (info) {
            // Doc-comment (doc 06 P2.3, interim LSP-side source rescan):
            // reuses getDeclarationLoc() -- same query doDefinition() uses
            // to find WHERE a symbol is declared -- to find WHAT file/line
            // to scan backward from. Only the node kinds that resolve a
            // real declaration site get one; a struct field/enum
            // member/module-name hover (which hover() answers but
            // getDeclarationLoc() doesn't resolve) simply gets none, which
            // is correct -- there's no single declaration line to attach a
            // "doc comment" to for those.
            if (auto declLocOpt = SemanticQuery::getDeclarationLoc(node);
                declLocOpt && declLocOpt->loc.Line > 0) {
                std::string docComment;
                if (declLocOpt->moduleName.empty()) {
                    docComment = readLeadingDocComment(splitLines(textSnap->text), declLocOpt->loc.Line);
                } else {
                    std::string docPath = uriToPath(uri);
                    std::string resolved = ResolveModuleFile(declLocOpt->moduleName, docPath);
                    if (!resolved.empty()) {
                        std::ifstream targetFile(resolved);
                        if (targetFile) {
                            std::stringstream buf;
                            buf << targetFile.rdbuf();
                            docComment = readLeadingDocComment(splitLines(buf.str()), declLocOpt->loc.Line);
                        }
                    }
                }
                if (!docComment.empty()) info->markdown += "\n\n---\n" + docComment;
            }
            result = {{"contents", {{"kind", "markdown"}, {"value", info->markdown}}}};
            if (info->length > 0) {
                result["range"] = jsonRange(mapper.toLspRange(info->loc.Line, info->loc.Col, info->length));
            }
        }
    }
    sendResponse(id, result);
}

void LSPHandler::doDefinition(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag) {
    std::string idKey = id.dump();
    ScopeExit cleanup{[this, idKey] { unregisterCancellable(idKey); }};

    auto cancelled = [&cancelFlag] { return cancelFlag && cancelFlag->load(); };
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!m_documents.contains(uri)) { sendError(id, -32801, "Content modified"); return; }

    auto analysis = ensureFreshAnalysis(uri, cancelled);
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!analysis) { sendResponse(id, nullptr); return; }

    auto textSnap = m_documents.getText(uri);
    if (!textSnap) { sendError(id, -32801, "Content modified"); return; }

    PositionMapper mapper(textSnap->text, m_positionEncoding);
    int compLine, compCol;
    mapper.toCompilerLineCol(pos, compLine, compCol);

    json result = nullptr;
    const ExprAST* node = SemanticQuery::findNodeAt(*analysis, compLine, compCol);
    if (node) {
        auto declLocOpt = SemanticQuery::getDeclarationLoc(node);
        if (declLocOpt && declLocOpt->loc.Line > 0) {
            std::string targetUri = uri;
            bool haveTarget = true;

            // Cross-file: resolve the declaring module's name to its real
            // on-disk file the same way a real `import` would -- NOT a
            // hardcoded flat `lib/std/<name>.tr` path, which broke the
            // moment stdlib modules were reorganized into subfolders (e.g.
            // `net` now lives at `lib/std/io/net.tr`). If resolution
            // fails, declLocOpt->loc is meaningless against `uri` (it's a
            // line/col in the UNRESOLVED target file) -- report no
            // location at all rather than point at the wrong file/line.
            if (!declLocOpt->moduleName.empty()) {
                std::string docPath = uriToPath(uri);
                std::string resolved = ResolveModuleFile(declLocOpt->moduleName, docPath);
                if (!resolved.empty()) {
                    targetUri = "file://" + resolved;
                } else {
                    haveTarget = false;
                }
            }

            if (haveTarget) {
                // `mapper` is built from THIS document's text, so it's
                // only valid for a same-file target. A cross-file target
                // (e.g. into a stdlib module) points at line/col in a
                // file we don't have the text of loaded here, so its
                // range falls back to the same raw 1-indexed -> 0-indexed
                // conversion the compiler's columns always resolved to
                // before PositionMapper existed -- correct as long as the
                // target identifier itself is ASCII, true for every
                // identifier turf-lang's lexer accepts. Only the same-file
                // case gets full UTF-16-aware conversion, since it's the
                // only one where a preceding non-ASCII character on the
                // line could otherwise throw the column off.
                // Target selection width now uses the real name length
                // (doc 07 P2.5) instead of a 1-char caret, wherever
                // getDeclarationLoc() knows it.
                int len = std::max(declLocOpt->length, 1);
                LspRange range;
                if (targetUri == uri) {
                    range = mapper.toLspRange(declLocOpt->loc.Line, declLocOpt->loc.Col, len);
                } else {
                    range.start = {declLocOpt->loc.Line - 1, declLocOpt->loc.Col - 1};
                    range.end = {declLocOpt->loc.Line - 1, declLocOpt->loc.Col - 1 + len};
                }
                result = buildDefinitionResult(*analysis, targetUri, range, m_linkSupport, node, compLine, compCol, mapper);
            }
        }
    }
    sendResponse(id, result);
}

// Same shape as doDefinition() above -- only the query (getTypeDefinitionLoc
// instead of getDeclarationLoc) and the range width differ. Kept as its own
// function rather than a shared helper parameterized on the query: the two
// are similar today but doc 07 expects them to diverge further (declaration
// vs. definition's extern-decl split doesn't apply here at all; typeDefinition
// has its own edge cases -- e.g. a builtin/primitive type has no decl node to
// find, already handled by getTypeDefinitionLoc() returning nullopt for it).
void LSPHandler::doTypeDefinition(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag) {
    std::string idKey = id.dump();
    ScopeExit cleanup{[this, idKey] { unregisterCancellable(idKey); }};

    auto cancelled = [&cancelFlag] { return cancelFlag && cancelFlag->load(); };
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!m_documents.contains(uri)) { sendError(id, -32801, "Content modified"); return; }

    auto analysis = ensureFreshAnalysis(uri, cancelled);
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!analysis) { sendResponse(id, nullptr); return; }

    auto textSnap = m_documents.getText(uri);
    if (!textSnap) { sendError(id, -32801, "Content modified"); return; }

    PositionMapper mapper(textSnap->text, m_positionEncoding);
    int compLine, compCol;
    mapper.toCompilerLineCol(pos, compLine, compCol);

    json result = nullptr;
    const ExprAST* node = SemanticQuery::findNodeAt(*analysis, compLine, compCol);
    if (node) {
        auto typeLocOpt = SemanticQuery::getTypeDefinitionLoc(*analysis, node);
        if (typeLocOpt && typeLocOpt->loc.Line > 0) {
            std::string targetUri = uri;
            bool haveTarget = true;

            // Same cross-file resolution as doDefinition() -- see its
            // identical comment above for the stdlib-subfolder rationale.
            if (!typeLocOpt->moduleName.empty()) {
                std::string docPath = uriToPath(uri);
                std::string resolved = ResolveModuleFile(typeLocOpt->moduleName, docPath);
                if (!resolved.empty()) {
                    targetUri = "file://" + resolved;
                } else {
                    haveTarget = false;
                }
            }

            if (haveTarget) {
                int len = std::max(typeLocOpt->length, 1);
                LspRange range;
                if (targetUri == uri) {
                    range = mapper.toLspRange(typeLocOpt->loc.Line, typeLocOpt->loc.Col, len);
                } else {
                    range.start = {typeLocOpt->loc.Line - 1, typeLocOpt->loc.Col - 1};
                    range.end = {typeLocOpt->loc.Line - 1, typeLocOpt->loc.Col - 1 + len};
                }
                result = buildDefinitionResult(*analysis, targetUri, range, m_linkSupport, node, compLine, compCol, mapper);
            }
        }
    }
    sendResponse(id, result);
}

// doc 08 P3.3: every occurrence of the symbol under the cursor, restricted
// to the current file -- no workspace scan needed, unlike doReferences()
// below. Read/Write distinction (doc's own edge case) isn't implemented --
// every highlight comes back plain (LSP `kind` omitted, which the spec
// defaults to Text=1), a real scope cut, not an oversight: telling a
// declaration/use apart from an assignment LHS specifically would need
// ReferenceVisitor to track one more bit per occurrence that nothing
// upstream currently computes.
void LSPHandler::doDocumentHighlight(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag) {
    std::string idKey = id.dump();
    ScopeExit cleanup{[this, idKey] { unregisterCancellable(idKey); }};

    auto cancelled = [&cancelFlag] { return cancelFlag && cancelFlag->load(); };
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!m_documents.contains(uri)) { sendError(id, -32801, "Content modified"); return; }

    auto analysis = ensureFreshAnalysis(uri, cancelled);
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!analysis) { sendResponse(id, nullptr); return; }

    auto textSnap = m_documents.getText(uri);
    if (!textSnap) { sendError(id, -32801, "Content modified"); return; }

    PositionMapper mapper(textSnap->text, m_positionEncoding);
    int compLine, compCol;
    mapper.toCompilerLineCol(pos, compLine, compCol);

    json result = nullptr;
    const ExprAST* node = SemanticQuery::findNodeAt(*analysis, compLine, compCol);
    if (node) {
        auto target = SemanticQuery::getDeclarationLoc(node);
        if (target && target->loc.Line > 0) {
            auto occurrences = SemanticQuery::findAllReferenceOccurrences(*analysis);
            json highlights = json::array();
            for (const auto& occ : occurrences) {
                if (occ.declLoc.Line == target->loc.Line && occ.declLoc.Col == target->loc.Col &&
                    occ.declModule == target->moduleName) {
                    highlights.push_back({{"range", jsonRange(mapper.toLspRange(occ.loc.Line, occ.loc.Col, occ.length))}});
                }
            }
            if (!highlights.empty()) result = highlights;
        }
    }
    sendResponse(id, result);
}

// doc 08 P3.5: workspace-wide references. Scans every file WorkspaceIndex
// enumerated (doc 01 P0.6) plus the current document, running a completely
// fresh, independent analyze() on each (never stored back via
// setAnalysis() -- these aren't the document being edited) and matching
// occurrences by identity: (declLoc, declModule resolved to an absolute
// path FROM THAT FILE's own position). No persistent OccurrenceTable /
// incremental invalidation (doc 08's P3.2/P3.4 as originally scoped) --
// a full per-request scan instead, which the plan doc itself calls out as
// "fine for typical project sizes" pending real dogfooding evidence it
// isn't.
void LSPHandler::doReferences(json id, std::string uri, LspPosition pos, bool includeDeclaration,
                               std::shared_ptr<std::atomic<bool>> cancelFlag) {
    std::string idKey = id.dump();
    ScopeExit cleanup{[this, idKey] { unregisterCancellable(idKey); }};

    auto cancelled = [&cancelFlag] { return cancelFlag && cancelFlag->load(); };
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!m_documents.contains(uri)) { sendError(id, -32801, "Content modified"); return; }

    auto analysis = ensureFreshAnalysis(uri, cancelled);
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!analysis) { sendResponse(id, nullptr); return; }

    auto textSnap = m_documents.getText(uri);
    if (!textSnap) { sendError(id, -32801, "Content modified"); return; }

    PositionMapper mapper(textSnap->text, m_positionEncoding);
    int compLine, compCol;
    mapper.toCompilerLineCol(pos, compLine, compCol);

    const ExprAST* node = SemanticQuery::findNodeAt(*analysis, compLine, compCol);
    if (!node) { sendResponse(id, json::array()); return; }
    auto target = SemanticQuery::getDeclarationLoc(node);
    if (!target || target->loc.Line <= 0) { sendResponse(id, json::array()); return; }

    // Resolve the target's own module to an absolute path ONCE, against
    // the document the reference request originated in -- correct for
    // every stdlib/usr-package target (steps 1-2 of ResolveModuleFile()'s
    // search order don't depend on the importing file at all) and for a
    // same-directory local-module target reached from a file in that same
    // directory; a target resolved only through some OTHER file's own
    // relative-directory step is the one case this can disagree with,
    // accepted as a known edge case rather than re-resolving per scanned
    // file (which would need each file's own import of the target to
    // already be known, a chicken-and-egg problem this simpler model
    // avoids).
    std::string docPath = uriToPath(uri);
    std::string targetFilePath = target->moduleName.empty() ? docPath
                                                              : ResolveModuleFile(target->moduleName, docPath);
    if (targetFilePath.empty()) { sendResponse(id, json::array()); return; }

    // Scan candidates: every workspace-indexed file, plus the target's own
    // declaring file and the current document (a target declared in a
    // file the WorkspaceIndex didn't enumerate -- e.g. outside every
    // workspace folder, or the index hasn't rescanned yet -- must still be
    // searched, since it's the one file GUARANTEED to contain at least the
    // declaration itself).
    std::vector<std::string> candidates = m_workspaceIndex.files();
    candidates.push_back(docPath);
    candidates.push_back(targetFilePath);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    json locations = json::array();
    for (const auto& candidatePath : candidates) {
        if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }

        const DocumentAnalysis* candidateAnalysis = nullptr;
        std::shared_ptr<const DocumentAnalysis> freshAnalysis; // keeps a fresh analyze() alive for this iteration
        std::string candidateUri = pathToUri(candidatePath);
        std::string candidateText;

        if (candidatePath == docPath) {
            candidateAnalysis = analysis.get();
            candidateText = textSnap->text;
        } else if (auto openSnap = m_documents.getText(candidateUri)) {
            // Already open in the editor -- reuse the live buffer's own
            // fresh analysis instead of re-reading it off disk stale.
            auto openAnalysis = ensureFreshAnalysis(candidateUri, cancelled);
            if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
            if (!openAnalysis) continue;
            freshAnalysis = openAnalysis;
            candidateAnalysis = openAnalysis.get();
            candidateText = openSnap->text;
        } else {
            std::ifstream f(candidatePath);
            if (!f) continue;
            std::stringstream buf;
            buf << f.rdbuf();
            candidateText = buf.str();
            auto analyzed = m_compiler.analyze(candidateText, candidatePath, cancelled);
            if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
            if (!analyzed) continue;
            freshAnalysis = std::shared_ptr<const DocumentAnalysis>(std::move(analyzed));
            candidateAnalysis = freshAnalysis.get();
        }

        auto occurrences = SemanticQuery::findAllReferenceOccurrences(*candidateAnalysis);
        if (occurrences.empty()) continue;

        PositionMapper candidateMapper(candidateText, m_positionEncoding);
        for (const auto& occ : occurrences) {
            if (occ.declLoc.Line != target->loc.Line || occ.declLoc.Col != target->loc.Col) continue;
            if (!includeDeclaration && occ.isDeclaration) continue;
            std::string occDeclFilePath = occ.declModule.empty() ? candidatePath
                                                                   : ResolveModuleFile(occ.declModule, candidatePath);
            if (occDeclFilePath != targetFilePath) continue;

            locations.push_back({
                {"uri", candidateUri},
                {"range", jsonRange(candidateMapper.toLspRange(occ.loc.Line, occ.loc.Col, occ.length))}
            });
        }
    }
    sendResponse(id, locations);
}

// doc 08 P3.6 (bonus): a flat outline of the current file's top-level
// struct/enum/function declarations as SymbolInformation (the simpler,
// non-hierarchical DocumentSymbol response variant -- nesting fields/
// methods under their owning struct is explicitly left for later, this
// covers the common "jump to a top-level declaration" case cheaply).
void LSPHandler::doDocumentSymbol(json id, std::string uri, std::shared_ptr<std::atomic<bool>> cancelFlag) {
    std::string idKey = id.dump();
    ScopeExit cleanup{[this, idKey] { unregisterCancellable(idKey); }};

    auto cancelled = [&cancelFlag] { return cancelFlag && cancelFlag->load(); };
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!m_documents.contains(uri)) { sendError(id, -32801, "Content modified"); return; }

    auto analysis = ensureFreshAnalysis(uri, cancelled);
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!analysis) { sendResponse(id, json::array()); return; }

    auto textSnap = m_documents.getText(uri);
    if (!textSnap) { sendError(id, -32801, "Content modified"); return; }
    PositionMapper mapper(textSnap->text, m_positionEncoding);

    json symbols = json::array();
    for (const auto& sym : SemanticQuery::getDocumentSymbols(*analysis)) {
        symbols.push_back({
            {"name", sym.name},
            {"kind", sym.kind},
            {"location", {
                {"uri", uri},
                {"range", jsonRange(mapper.toLspRange(sym.loc.Line, sym.loc.Col, std::max(sym.length, 1)))}
            }}
        });
    }
    sendResponse(id, symbols);
}

// doc 09 P4.3: textDocument/formatting, reusing turf-lang's
// turf::format::format() in-process (the SAME implementation `turf
// --format` uses -- see that engine's own doc comment for the "one
// implementation, two callers" rationale). Ships the simplest CORRECT
// response shape: a single TextEdit spanning the whole document, not a
// minimal line-level diff (doc 09's own text: "A single full-replace
// edit is *allowed* but churns the client's cursor/undo; prefer a
// minimal diff" -- explicitly allowed, just not the nicer option; a
// real scope cut for this pass, not an oversight). No analyze() call
// needed at all -- formatting is a pure function of the raw text, so
// this doesn't even touch the worker's usual ensureFreshAnalysis() path.
void LSPHandler::doFormatting(json id, std::string uri, bool insertSpaces, int tabSize,
                               std::shared_ptr<std::atomic<bool>> cancelFlag) {
    std::string idKey = id.dump();
    ScopeExit cleanup{[this, idKey] { unregisterCancellable(idKey); }};

    auto cancelled = [&cancelFlag] { return cancelFlag && cancelFlag->load(); };
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!m_documents.contains(uri)) { sendError(id, -32801, "Content modified"); return; }

    auto textSnap = m_documents.getText(uri);
    if (!textSnap) { sendError(id, -32801, "Content modified"); return; }

    turf::format::FormatOptions opts;
    opts.InsertSpaces = insertSpaces;
    opts.IndentWidth = tabSize > 0 ? tabSize : 4;
    auto result = turf::format::format(textSnap->text, opts);

    if (!result.Ok || result.Text == textSnap->text) {
        sendResponse(id, json::array());
        return;
    }

    PositionMapper mapper(textSnap->text, m_positionEncoding);
    LspPosition endPos = mapper.toLspPosition(textSnap->text.size());
    json edit = {
        {"range", jsonRange({{0, 0}, endPos})},
        {"newText", result.Text}
    };
    sendResponse(id, json::array({edit}));
}

std::vector<int> LSPHandler::computeSemanticTokenData(const DocumentAnalysis& analysis,
                                                        const PositionMapper& mapper) const {
    auto tokens = SemanticQuery::getSemanticTokens(analysis);

    std::vector<int> data;
    int prevLine = 0;
    int prevChar = 0;
    for (const auto& token : tokens) {
        // Identifiers are ASCII-only in turf-lang's lexer, so a token's
        // byte length equals its length in the negotiated encoding's
        // units too -- only the START position needs real conversion.
        LspPosition p = mapper.toLspRange(token.line, token.col, 0).start;
        int deltaLine = p.line - prevLine;
        int deltaStartChar = (deltaLine == 0) ? (p.character - prevChar) : p.character;

        data.push_back(deltaLine);
        data.push_back(deltaStartChar);
        data.push_back(token.length);
        data.push_back(static_cast<int>(token.type));
        data.push_back(token.modifiers);

        prevLine = p.line;
        prevChar = p.character;
    }
    return data;
}

void LSPHandler::doSemanticTokens(json id, std::string uri, std::shared_ptr<std::atomic<bool>> cancelFlag) {
    std::string idKey = id.dump();
    ScopeExit cleanup{[this, idKey] { unregisterCancellable(idKey); }};

    auto cancelled = [&cancelFlag] { return cancelFlag && cancelFlag->load(); };
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!m_documents.contains(uri)) { sendError(id, -32801, "Content modified"); return; }

    auto analysis = ensureFreshAnalysis(uri, cancelled);
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!analysis) { sendResponse(id, nullptr); return; }

    auto textSnap = m_documents.getText(uri);
    if (!textSnap) { sendError(id, -32801, "Content modified"); return; }

    PositionMapper mapper(textSnap->text, m_positionEncoding);
    std::vector<int> data = computeSemanticTokenData(*analysis, mapper);

    // Cached for a later full/delta request against this same uri (doc 04
    // P1.d) -- worker-thread-only state, see the member's own doc comment.
    std::string resultId = std::to_string(++m_nextSemanticTokensResultId);
    m_semanticTokensCache[uri] = CachedSemanticTokens{resultId, data};

    sendResponse(id, {{"resultId", resultId}, {"data", data}});
}

void LSPHandler::doSemanticTokensDelta(json id, std::string uri, std::string previousResultId,
                                        std::shared_ptr<std::atomic<bool>> cancelFlag) {
    std::string idKey = id.dump();
    ScopeExit cleanup{[this, idKey] { unregisterCancellable(idKey); }};

    auto cancelled = [&cancelFlag] { return cancelFlag && cancelFlag->load(); };
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!m_documents.contains(uri)) { sendError(id, -32801, "Content modified"); return; }

    auto analysis = ensureFreshAnalysis(uri, cancelled);
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!analysis) { sendResponse(id, nullptr); return; }

    auto textSnap = m_documents.getText(uri);
    if (!textSnap) { sendError(id, -32801, "Content modified"); return; }

    PositionMapper mapper(textSnap->text, m_positionEncoding);
    std::vector<int> newData = computeSemanticTokenData(*analysis, mapper);
    std::string newResultId = std::to_string(++m_nextSemanticTokensResultId);

    auto cacheIt = m_semanticTokensCache.find(uri);
    bool havePrevious = cacheIt != m_semanticTokensCache.end() && cacheIt->second.resultId == previousResultId;

    json result;
    if (!havePrevious) {
        // Per spec: the server may return a full SemanticTokens result
        // instead of a delta whenever it can't (or chooses not to) compute
        // one against `previousResultId` -- e.g. a cold cache after a
        // server restart, or the client citing a resultId this server
        // never issued (a stale client, a raced full/delta interleaving).
        result = {{"resultId", newResultId}, {"data", newData}};
    } else {
        const std::vector<int>& oldData = cacheIt->second.data;
        size_t minLen = std::min(oldData.size(), newData.size());
        size_t prefix = 0;
        while (prefix < minLen && oldData[prefix] == newData[prefix]) ++prefix;
        size_t suffix = 0;
        while (suffix < minLen - prefix &&
               oldData[oldData.size() - 1 - suffix] == newData[newData.size() - 1 - suffix]) {
            ++suffix;
        }
        size_t deleteCount = oldData.size() - prefix - suffix;
        std::vector<int> middle(newData.begin() + static_cast<long>(prefix),
                                 newData.end() - static_cast<long>(suffix));

        json edits = json::array();
        if (deleteCount != 0 || !middle.empty()) {
            edits.push_back({{"start", prefix}, {"deleteCount", deleteCount}, {"data", middle}});
        }
        result = {{"resultId", newResultId}, {"edits", edits}};
    }

    m_semanticTokensCache[uri] = CachedSemanticTokens{newResultId, std::move(newData)};
    sendResponse(id, result);
}

void LSPHandler::doCompletion(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag) {
    std::string idKey = id.dump();
    ScopeExit cleanup{[this, idKey] { unregisterCancellable(idKey); }};

    auto cancelled = [&cancelFlag] { return cancelFlag && cancelFlag->load(); };
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!m_documents.contains(uri)) { sendError(id, -32801, "Content modified"); return; }

    json items = json::array();
    auto textSnap = m_documents.getText(uri);
    if (textSnap) {
        PositionMapper mapper(textSnap->text, m_positionEncoding);
        size_t lineStartByte = mapper.lineStart(pos.line);
        size_t cursorByte = mapper.toByteOffset(pos);
        std::string prefixText = lineTextUpToByteOffset(textSnap->text, lineStartByte, cursorByte);
        int compLine, compCol;
        mapper.toCompilerLineCol(pos, compLine, compCol);

        // `import mod::<prefix>` (doc 05 G2's other half): `mod` isn't
        // imported into THIS document yet -- that's literally what this
        // statement is doing -- so its exported symbols can't come from
        // this document's own symbol table at all, unlike every other
        // trigger kind below. Detected and handled entirely separately,
        // before any of that machinery runs.
        std::string importSymbolModule;
        if (SemanticQuery::isImportSymbolTrigger(prefixText, importSymbolModule)) {
            std::vector<CompletionItem> completions;
            std::string resolved = ResolveModuleFile(importSymbolModule, uriToPath(uri));
            if (!resolved.empty()) {
                std::ifstream targetFile(resolved);
                if (targetFile) {
                    std::stringstream buf;
                    buf << targetFile.rdbuf();
                    // A fresh, independent analysis of the TARGET module's
                    // own file -- never stored back via setAnalysis(),
                    // since this uri isn't even the document being
                    // completed.
                    auto moduleAnalysis = m_compiler.analyze(buf.str(), resolved, cancelled);
                    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
                    if (moduleAnalysis) completions = SemanticQuery::getModuleExportedSymbols(*moduleAnalysis);
                }
            }
            for (const auto& item : completions) {
                json lspItem = {{"label", item.label}, {"kind", item.kind}};
                if (!item.detail.empty()) lspItem["detail"] = item.detail;
                if (!item.insertSnippet.empty()) {
                    lspItem["insertText"] = item.insertSnippet;
                    lspItem["insertTextFormat"] = 2; // Snippet
                }
                items.push_back(std::move(lspItem));
            }
            sendResponse(id, {{"isIncomplete", false}, {"items", items}});
            return;
        }

        // Try the already-cached (and, since ensureFreshAnalysis self-
        // heals, always up-to-date-or-refreshed) analysis first for an
        // `Ident::` trigger specifically, instead of always paying for a
        // second full analyze() per completion request. This is the one
        // trigger shape where it's actually safe: `::`'s base is always a
        // module, struct, or enum name (turf-lang has no local-scoped
        // equivalent), and all three are registered from their OWN
        // separate top-level declaration (`import X;` / `struct X {}` /
        // `enum X {}`) -- resolving `Ident` never depends on whether the
        // CURRENT (possibly still-being-typed, unterminated) statement's
        // enclosing function parsed successfully. A bare-word or `.`
        // (MemberAccess) trigger does NOT get this treatment: both
        // commonly resolve against a LOCAL variable declared earlier in
        // the very same function this statement is inside, and turf-
        // lang's recovery only resyncs at TOP-LEVEL statement boundaries
        // (not within a function body) -- an unterminated in-progress
        // statement anywhere in that function (exactly the buffer's live
        // state while the user is mid-typing) drops the WHOLE function's
        // locals from the cached analysis. Those two trigger kinds always
        // go through the patched reanalysis below.
        bool isScopeAccessTrigger = false;
        {
            int p = static_cast<int>(prefixText.size());
            while (p > 0 && isIdentChar(prefixText[p - 1])) p--;
            isScopeAccessTrigger = p >= 2 && prefixText[p - 1] == ':' && prefixText[p - 2] == ':';
        }

        std::vector<CompletionItem> completions;
        if (isScopeAccessTrigger) {
            auto analysis = ensureFreshAnalysis(uri, cancelled);
            if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
            if (analysis) completions = SemanticQuery::getCompletions(*analysis, prefixText, "", compLine);
        }

        if (!isScopeAccessTrigger || completions.empty()) {
            std::string path = uriToPath(uri);
            std::string patchedText = patchForCompletionAt(textSnap->text, lineStartByte, cursorByte);
            // Throwaway analysis for this one request only -- never stored
            // back via setAnalysis(), so the document's real cached
            // analysis (and the user's actual unmodified text) is
            // untouched by the `turfLspCursor;` patch above.
            auto completionAnalysis = m_compiler.analyze(patchedText, path, cancelled);
            if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
            if (completionAnalysis) {
                completions = SemanticQuery::getCompletions(*completionAnalysis, prefixText, path, compLine);
            }
        }

        for (const auto& item : completions) {
            json lspItem = {{"label", item.label}, {"kind", item.kind}};
            if (!item.detail.empty()) lspItem["detail"] = item.detail;
            if (!item.insertSnippet.empty()) {
                lspItem["insertText"] = item.insertSnippet;
                lspItem["insertTextFormat"] = 2; // Snippet
            }
            if (!item.sortText.empty()) lspItem["sortText"] = item.sortText;
            // doc 05 G4: carries just enough to re-resolve this exact
            // symbol later, without eagerly paying for the doc-comment
            // file read every item in a (possibly large) list would cost.
            // Omitted entirely for items with nothing to resolve (keywords,
            // types, locals, ...) so the client never bothers asking.
            if (!item.resolveName.empty()) {
                lspItem["data"] = {{"uri", uri}, {"name", item.resolveName}, {"module", item.resolveModule}};
            }
            items.push_back(std::move(lspItem));
        }
    }

    sendResponse(id, {{"isIncomplete", false}, {"items", items}});
}

// doc 05 G4: fills in `documentation` (the function/method's doc-comment)
// on a single completion item, lazily -- doCompletion() deliberately never
// pays for this eagerly (an extra source scan, possibly a cross-file disk
// read, per item in a list that can have dozens of entries). Returns
// `item` completely unmodified if its `data` is missing/malformed, or if
// the name no longer resolves (the document changed between the original
// completion request and this one) -- a resolve response must be a
// complete CompletionItem, so silently dropping fields on failure would be
// wrong; returning the original item verbatim is the correct "nothing to
// add" behavior.
void LSPHandler::doCompletionResolve(json id, json item, std::shared_ptr<std::atomic<bool>> cancelFlag) {
    std::string idKey = id.dump();
    ScopeExit cleanup{[this, idKey] { unregisterCancellable(idKey); }};

    auto cancelled = [&cancelFlag] { return cancelFlag && cancelFlag->load(); };
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }

    if (!item.contains("data") || !item["data"].is_object()) { sendResponse(id, item); return; }
    std::string uri = item["data"].value("uri", "");
    std::string name = item["data"].value("name", "");
    std::string module = item["data"].value("module", "");
    if (uri.empty() || name.empty() || !m_documents.contains(uri)) { sendResponse(id, item); return; }

    auto analysis = ensureFreshAnalysis(uri, cancelled);
    if (cancelled()) { sendError(id, -32800, "Request cancelled"); return; }
    if (!analysis) { sendResponse(id, item); return; }

    auto declLocOpt = SemanticQuery::getFunctionDeclLoc(*analysis, name, module);
    if (!declLocOpt || declLocOpt->loc.Line <= 0) { sendResponse(id, item); return; }

    // Same same-file-vs-cross-file doc-comment strategy doHover() uses.
    std::string docComment;
    if (module.empty()) {
        auto textSnap = m_documents.getText(uri);
        if (textSnap) docComment = readLeadingDocComment(splitLines(textSnap->text), declLocOpt->loc.Line);
    } else {
        std::string resolved = ResolveModuleFile(module, uriToPath(uri));
        if (!resolved.empty()) {
            std::ifstream targetFile(resolved);
            if (targetFile) {
                std::stringstream buf;
                buf << targetFile.rdbuf();
                docComment = readLeadingDocComment(splitLines(buf.str()), declLocOpt->loc.Line);
            }
        }
    }
    if (!docComment.empty()) {
        item["documentation"] = {{"kind", "markdown"}, {"value", docComment}};
    }
    sendResponse(id, item);
}

void LSPHandler::sendMessage(const json& message) {
    std::string serialized = message.dump();
    std::lock_guard<std::mutex> lock(m_outMutex);
    std::cout << "Content-Length: " << serialized.length() << "\r\n\r\n" << serialized;
    std::cout.flush();
}

void LSPHandler::sendResponse(const json& id, const json& result) {
    sendMessage({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

void LSPHandler::sendError(const json& id, int code, const std::string& message) {
    sendMessage({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}
