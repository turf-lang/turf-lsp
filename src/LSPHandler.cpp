#include "LSPHandler.hpp"
#include "SemanticQuery.hpp"
#include "Logger.hpp"
#include "Parser.h"
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
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

json jsonRange(const LspRange& r) {
    return {
        {"start", {{"line", r.start.line}, {"character", r.start.character}}},
        {"end", {{"line", r.end.line}, {"character", r.end.character}}}
    };
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
    if (method == "textDocument/hover") { handleHover(msg); return; }
    if (method == "textDocument/definition") { handleDefinition(msg); return; }
    if (method == "textDocument/declaration") { handleDeclaration(msg); return; }
    if (method == "textDocument/typeDefinition") { handleTypeDefinition(msg); return; }
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
            {"semanticTokensProvider", {
                {"legend", {
                    {"tokenTypes", {"variable", "function", "keyword", "type", "parameter", "struct", "string", "number", "operator", "namespace", "property", "enumMember", "method"}},
                    {"tokenModifiers", {"declaration", "readonly"}}
                }},
                {"full", {{"delta", true}}}
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
        auto info = SemanticQuery::hover(node, compLine, compCol);
        if (info) {
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
                LspRange range;
                if (targetUri == uri) {
                    range = mapper.toLspRange(declLocOpt->loc.Line, declLocOpt->loc.Col, 1);
                } else {
                    range.start = {declLocOpt->loc.Line - 1, declLocOpt->loc.Col - 1};
                    range.end = {declLocOpt->loc.Line - 1, declLocOpt->loc.Col};
                }
                result = {
                    {"uri", targetUri},
                    {"range", {
                        {"start", {{"line", range.start.line}, {"character", range.start.character}}},
                        {"end", {{"line", range.end.line}, {"character", range.end.character}}}
                    }}
                };
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
                LspRange range;
                if (targetUri == uri) {
                    range = mapper.toLspRange(typeLocOpt->loc.Line, typeLocOpt->loc.Col, 1);
                } else {
                    range.start = {typeLocOpt->loc.Line - 1, typeLocOpt->loc.Col - 1};
                    range.end = {typeLocOpt->loc.Line - 1, typeLocOpt->loc.Col};
                }
                result = {
                    {"uri", targetUri},
                    {"range", {
                        {"start", {{"line", range.start.line}, {"character", range.start.character}}},
                        {"end", {{"line", range.end.line}, {"character", range.end.character}}}
                    }}
                };
            }
        }
    }
    sendResponse(id, result);
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
            if (analysis) completions = SemanticQuery::getCompletions(*analysis, prefixText);
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
                completions = SemanticQuery::getCompletions(*completionAnalysis, prefixText, path);
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
    }

    sendResponse(id, {{"isIncomplete", false}, {"items", items}});
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
