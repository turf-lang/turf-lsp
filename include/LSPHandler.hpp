#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

#include "CompilerService.hpp"
#include "DocumentStore.hpp"
#include "PositionMapper.hpp"
#include "StdlibLocator.hpp"
#include "TextFeatures.hpp"
#include "WorkspaceIndex.hpp"

using json = nlohmann::json;

// Two-thread model: the I/O thread (whatever calls
// handleMessage) only frames/parses JSON-RPC and does cheap, non-compiler
// bookkeeping (updating DocumentStore's text, cancellation flags,
// lifecycle state) -- it never blocks on an `analyze()` call. Anything that
// touches the compiler (CompilerService::analyze(), SemanticQuery over a
// fresh/patched analysis, ResolveModuleFile()'s module-search cache) runs
// exclusively on one dedicated worker thread, because the compiler was
// written as a single-process batch tool: its lexer/parser cursor, symbol
// tables, diagnostics and recovery jump buffer are all plain process-global
// mutable state. turf::analyze() (the compiler's query facade) saves,
// isolates and restores that state around each analysis and takes its own
// lock, but module resolution (ResolveModuleFile(), ListAvailableModules())
// and the SemanticQuery walks over an analysis's AST are outside it, so one
// worker still owns all compiler access and keeps it serialized.
class LSPHandler {
public:
    LSPHandler();
    ~LSPHandler();

    // Parses a JSON-RPC message and dispatches it. Called from the I/O
    // thread; returns quickly (never runs compiler code itself -- see the
    // class comment).
    void handleMessage(const std::string& message);

    // Where the standard library was found at startup (or that it was not),
    // so the user is told once the client is listening: a server with no
    // stdlib answers every `import` with an error, and the cause is not
    // something a diagnostic on line 1 explains well.
    void setStdlib(turf_lsp::StdlibLocation location) { m_stdlib = std::move(location); }

    // Where `turf` is (see ProjectGraph.hpp); empty analyses every file alone.
    // Call before the first message: the worker thread owns it afterwards.
    void setTurfExecutable(std::string turfExe) { m_compiler.setTurfExecutable(std::move(turfExe)); }

private:
    turf_lsp::StdlibLocation m_stdlib;

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
    void handleSignatureHelp(const json& request);
    void handleInlayHint(const json& request);
    void handleWorkspaceSymbol(const json& request);
    void handleDefinition(const json& request);
    // `declaration` reuses doDefinition() verbatim as its work body -- for
    // turf-lang, declaration and definition coincide for every symbol kind
    // doDefinition() currently resolves (no separate extern-decl split
    // implemented yet; see doTypeDefinition's sibling doc comment).
    void handleDeclaration(const json& request);
    void handleTypeDefinition(const json& request);
    void handleDocumentHighlight(const json& request);
    void handleReferences(const json& request);
    void handleDocumentSymbol(const json& request);
    void handleFormatting(const json& request);
    void handleRangeFormatting(const json& request);
    void handleDidChangeWatchedFiles(const json& notification);
    void handleFoldingRange(const json& request);
    void handleDocumentLink(const json& request);
    void handlePrepareRename(const json& request);
    void handleRename(const json& request);
    void handleCodeAction(const json& request);
    void handleSemanticTokens(const json& request);
    void handleSemanticTokensDelta(const json& request);

    // ---- Work bodies (worker thread only) ----
    void doCompletion(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag);
    // Unlike every other do*(), this one doesn't need a document
    // position -- it re-derives its target purely from the `data` payload
    // doCompletion() attached to the item being resolved.
    void doCompletionResolve(json id, json item, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doHover(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doSignatureHelp(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doInlayHint(json id, std::string uri, LspRange range, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doWorkspaceSymbol(json id, std::string query, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doDefinition(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doTypeDefinition(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doDocumentHighlight(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doReferences(json id, std::string uri, LspPosition pos, bool includeDeclaration,
                       std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doDocumentSymbol(json id, std::string uri, std::shared_ptr<std::atomic<bool>> cancelFlag);
    // `rangeStartLine`/`rangeEndLine` (0-based, inclusive; -1 = the whole
    // document): textDocument/rangeFormatting asks for edits only in those
    // lines.
    void doFormatting(json id, std::string uri, bool insertSpaces, int tabSize,
                       std::shared_ptr<std::atomic<bool>> cancelFlag, int rangeStartLine = -1,
                       int rangeEndLine = -1);
    std::optional<json> importAction(const std::string& uri, const std::string& statement, const json& diag);
    void doFoldingRange(json id, std::string uri, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doDocumentLink(json id, std::string uri, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doPrepareRename(json id, std::string uri, LspPosition pos, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doRename(json id, std::string uri, LspPosition pos, std::string newName,
                  std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doCodeAction(json id, std::string uri, json diagnostics,
                       std::shared_ptr<std::atomic<bool>> cancelFlag);

    // A single occurrence of the symbol a references/rename request is
    // asking about, already resolved to the file it was found in --
    // shared by doReferences() and doRename() (rename IS find-references
    // + text replacement) so the workspace-scanning
    // logic (candidate file enumeration, per-file fresh-vs-cached
    // analysis, cross-file module-to-path resolution) exists in exactly
    // one place.
    struct FoundOccurrence {
        std::string uri;
        SourceLocation loc;
        int length;
        bool isDeclaration;
    };
    // Returns nullopt if the position doesn't resolve to a renameable/
    // referenceable symbol at all (caller should respond accordingly);
    // the vector is empty only if resolution succeeded but scanning found
    // literally nothing (shouldn't normally happen -- the declaration
    // itself should always be found). Sets `outCancelled` and returns
    // nullopt if a cancellation was observed mid-scan; the caller is
    // responsible for having already sent (or now sending) the
    // `-32800` error in that case, this helper does not send responses
    // itself.
    std::optional<std::vector<FoundOccurrence>> collectWorkspaceOccurrences(
        const std::string& uri, LspPosition pos, const std::function<bool()>& cancelled, bool& outCancelled);
    void doSemanticTokens(json id, std::string uri, std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doSemanticTokensDelta(json id, std::string uri, std::string previousResultId,
                                std::shared_ptr<std::atomic<bool>> cancelFlag);
    void doPublishDiagnostics(std::string uri);

    // Shared by doSemanticTokens/doSemanticTokensDelta: builds the flat,
    // already-delta-encoded (per the LSP data-array format, not to be
    // confused with the full/delta REQUEST) [deltaLine, deltaStartChar,
    // length, tokenType, tokenModifiers] quintuples for `analysis`, with
    // positions converted through `mapper`.
    std::vector<int> computeSemanticTokenData(const DocumentAnalysis& analysis, const PositionMapper& mapper) const;

    // Returns the cached analysis if it's already for the document's
    // current text version, otherwise runs a fresh analyze() (worker
    // thread only) and stores it back. This is what makes on-demand
    // requests correct without needing them to wait on the debounce timer
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

    // ---- Cancellation ----
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
    // From capabilities.textDocument.definition.linkSupport --
    // used for declaration/typeDefinition too, on the pragmatic assumption
    // (true of every real client: VS Code, Neovim's vim.lsp, ...) that a
    // client declaring LocationLink support for one of the three declares
    // it for all three, rather than storing three separate flags for a
    // distinction no real client actually makes.
    // Declarations of files that are not open, for workspace symbol search,
    // keyed by path and re-read when the file's modification time changes.
    // Worker thread only.
    struct CachedDeclarations {
        long long modified = 0;
        std::vector<turf_lsp::Declaration> declarations;
    };
    std::unordered_map<std::string, CachedDeclarations> m_declarationCache;

    bool m_linkSupport = false;
    // initializationOptions from the client (the `turf.diagnostics.enable` and
    // `turf.completion.snippets` settings); changing either restarts the server.
    std::atomic<bool> m_diagnosticsEnabled{true};
    std::atomic<bool> m_completionSnippets{true};

    // Worker thread + single-consumer queue.
    std::thread m_workerThread;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<std::function<void()>> m_queue;
    bool m_stopWorker = false;

    // Debounce thread: coalesces rapid didChange bursts into one
    // publishDiagnostics per ~debounce-window. Keyed by
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
    // clear its now-stale squiggle. Touched from both the
    // worker thread (doPublishDiagnostics) and the I/O thread
    // (handleDidClose publishes directly, bypassing the queue, for prompt
    // squiggle-clearing on close) -- hence its own mutex.
    std::mutex m_diagMutex;
    std::unordered_map<std::string, std::set<std::string>> m_secondaryDiagUris;

    // Last full semantic-tokens result per document uri, for
    // textDocument/semanticTokens/full/delta. Worker-thread-
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
