#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

#include "CompilerService.hpp"
#include "DocumentStore.hpp"
#include "PositionMapper.hpp"
#include "WorkspaceIndex.hpp"

using json = nlohmann::json;

// Two-thread model (doc 01 P0.1): the I/O thread (whatever calls
// handleMessage) only frames/parses JSON-RPC and does cheap, non-compiler
// bookkeeping (updating DocumentStore's text, cancellation flags,
// lifecycle state) -- it never blocks on an `analyze()` call. Anything that
// touches the compiler (CompilerService::analyze(), SemanticQuery over a
// fresh/patched analysis, ResolveModuleFile()'s module-search cache) runs
// exclusively on one dedicated worker thread, because the compiler was
// written as a single-process batch tool: its lexer/parser cursor,
// GlobalSymbolTable, DiagnosticEngine, and the recovery jump buffer are all
// plain process-global mutable state with no locking of their own. Two
// analyses running concurrently would corrupt each other; one worker owning
// all of it serializes access without needing to touch the compiler itself.
class LSPHandler {
public:
    LSPHandler();
    ~LSPHandler();

    // Parses a JSON-RPC message and dispatches it. Called from the I/O
    // thread; returns quickly (never runs compiler code itself -- see the
    // class comment).
    void handleMessage(const std::string& message);

private:
    // ---- Dispatch (I/O thread) ----
    void dispatch(const json& msg);
    bool isRequest(const json& msg) const;

    // ---- Lifecycle & document sync (I/O thread; no compiler access) ----
    void handleInitialize(const json& request);
    void handleInitialized(const json& notification);
    void handleShutdown(const json& request);
    void handleExit();
    void handleDidOpen(const json& notification);
    void handleDidChange(const json& notification);
    void handleDidClose(const json& notification);
    void handleCancelRequest(const json& notification);

    // ---- Request entry points (I/O thread: parse params, enqueue work) ----
    void handleCompletion(const json& request);
    void handleCompletionResolve(const json& request);
    void handleHover(const json& request);
    void handleDefinition(const json& request);
    // `declaration` reuses doDefinition() verbatim as its work body -- for
    // turf-lang, declaration and definition coincide for every symbol kind
    // doDefinition() currently resolves (no separate extern-decl split
    // implemented yet; see doTypeDefinition's sibling doc comment).
    void handleDeclaration(const json& request);
    void handleTypeDefinition(const json& request);
    void handleSemanticTokens(const json& request);
    void handleSemanticTokensDelta(const json& request);

    // ---- Work bodies (worker thread only) ----
    void doCompletion(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag);
    // Unlike every other do*(), this one doesn't need a document
    // position -- it re-derives its target purely from the `data` payload
    // doCompletion() attached to the item being resolved.
    void doCompletionResolve(json id, json item, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doHover(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doDefinition(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doTypeDefinition(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doSemanticTokens(json id, std::string uri, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doSemanticTokensDelta(json id, std::string uri, std::string previousResultId,
                                std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doPublishDiagnostics(std::string uri);

    // Shared by doSemanticTokens/doSemanticTokensDelta: builds the flat,
    // already-delta-encoded (per the LSP data-array format, not to be
    // confused with the full/delta REQUEST) [deltaLine, deltaStartChar,
    // length, tokenType, tokenModifiers] quintuples for `analysis`, with
    // positions converted through `mapper` (doc 04 P1.b/P1.d).
    std::vector<int> computeSemanticTokenData(const DocumentAnalysis& analysis, const PositionMapper& mapper) const;

    // Returns the cached analysis if it's already for the document's
    // current text version, otherwise runs a fresh analyze() (worker
    // thread only) and stores it back. This is what makes on-demand
    // requests correct without needing them to wait on the debounce timer
    // (doc 01 P0.1's "always run against the latest analyzed version"):
    // rather than coordinate with the debounce, each request just
    // guarantees its own freshness the moment it runs.
    std::shared_ptr<const DocumentAnalysis> ensureFreshAnalysis(const std::string& uri,
                                                                  const std::function<bool()>& isCancelled = {});

    // ---- Worker thread (owns all compiler access) ----
    void enqueueWork(std::function<void()> task);
    void workerLoop();

    // ---- Debounce thread (schedules diagnostics re-publication) ----
    void scheduleDebouncedDiagnostics(const std::string& uri, int64_t version);
    void debounceLoop();

    // ---- Cancellation (doc 01 P0.4) ----
    std::shared_ptr<std::atomic<bool>> registerCancellable(const json& id);
    void unregisterCancellable(const std::string& idKey);

    // ---- Outgoing messages (thread-safe: both worker and I/O thread write) ----
    void sendMessage(const json& message);
    void sendResponse(const json& id, const json& result);
    void sendError(const json& id, int code, const std::string& message);

    std::string uriToPath(const std::string& uri) const;
    std::string pathToUri(const std::string& path) const;

    DocumentStore m_documents;
    CompilerService m_compiler;
    WorkspaceIndex m_workspaceIndex;

    bool m_initialized = false;
    bool m_shutdownRequested = false;
    PositionEncodingKind m_positionEncoding = PositionEncodingKind::UTF16;
    // From capabilities.textDocument.definition.linkSupport (doc 07 P2.2) --
    // used for declaration/typeDefinition too, on the pragmatic assumption
    // (true of every real client: VS Code, Neovim's vim.lsp, ...) that a
    // client declaring LocationLink support for one of the three declares
    // it for all three, rather than storing three separate flags for a
    // distinction no real client actually makes.
    bool m_linkSupport = false;

    // Worker thread + single-consumer queue.
    std::thread m_workerThread;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<std::function<void()>> m_queue;
    bool m_stopWorker = false;

    // Debounce thread: coalesces rapid didChange bursts into one
    // publishDiagnostics per ~debounce-window, per doc 01 P0.1. Keyed by
    // uri; a new edit simply overwrites the pending entry's due time
    // (naturally coalescing) rather than queuing a second timer.
    struct PendingDebounce {
        int64_t version;
        std::chrono::steady_clock::time_point dueAt;
    };
    std::thread m_debounceThread;
    std::mutex m_debounceMutex;
    std::condition_variable m_debounceCv;
    std::unordered_map<std::string, PendingDebounce> m_pendingDebounce;
    bool m_stopDebounce = false;
    std::chrono::milliseconds m_debounceDelay;

    // Serializes writes to stdout: both the I/O thread (responses to
    // lifecycle requests) and the worker thread (responses to
    // hover/completion/etc., plus diagnostics notifications) call
    // sendMessage().
    std::mutex m_outMutex;

    // request id (its json::dump()) -> cancellation flag, for in-flight or
    // still-queued cancellable requests (completion/hover/definition/
    // semanticTokens).
    std::mutex m_cancelMutex;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> m_cancelFlags;

    // Tracks, per primary document uri, the set of OTHER (imported-module)
    // uris its last diagnostics publish sent non-empty diagnostics to --
    // diffed against the next publish so a diagnostic that moves or
    // disappears from an imported file still gets an empty-array publish to
    // clear its now-stale squiggle (doc 03 P1.4). Touched from both the
    // worker thread (doPublishDiagnostics) and the I/O thread
    // (handleDidClose publishes directly, bypassing the queue, for prompt
    // squiggle-clearing on close) -- hence its own mutex.
    std::mutex m_diagMutex;
    std::unordered_map<std::string, std::set<std::string>> m_secondaryDiagUris;

    // Last full semantic-tokens result per document uri, for
    // textDocument/semanticTokens/full/delta (doc 04 P1.d). Worker-thread-
    // only (doSemanticTokens/doSemanticTokensDelta are the sole readers and
    // writers, and both only ever run as enqueued worker-thread work), so
    // unlike m_documents/m_secondaryDiagUris this needs no mutex of its own.
    struct CachedSemanticTokens {
        std::string resultId;
        std::vector<int> data;
    };
    std::unordered_map<std::string, CachedSemanticTokens> m_semanticTokensCache;
    int64_t m_nextSemanticTokensResultId = 0;
};
