#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "DocumentAnalysis.hpp"
#include "PositionMapper.hpp"

// Replaces the old ad-hoc `unordered_map<uri, DocumentState>` that lived
// directly on LSPHandler. Two things changed:
//
//   1. Thread safety. The I/O thread mutates document text the instant a
//      didChange notification is parsed (so a hover/completion request
//      queued right behind it sees the latest text even before the worker
//      thread gets around to re-analyzing); the worker thread reads text to
//      analyze and writes the resulting analysis back. Both happen from
//      different threads now, so access needs a lock -- but a short one:
//      analyze() itself must never run while holding it (that would block
//      the I/O thread's next didChange behind however long analysis takes,
//      defeating the entire point of splitting the threads).
//
//   2. `shared_ptr<const DocumentAnalysis>` instead of `unique_ptr`, so a
//      handler can grab a stable reference to "the analysis as of when my
//      request started" and keep using it after releasing the lock, even
//      if the worker replaces the stored analysis with a newer one in the
//      meantime -- readers never block writers and never see a half-updated
//      object.
struct OpenDocument {
    std::string uri;
    std::string text;
    int64_t version = 0;
    std::string languageId = "turf";
    std::shared_ptr<const DocumentAnalysis> analysis;
    int64_t analyzedVersion = -1; // version `analysis` was computed from; -1 = none yet
};

class DocumentStore {
public:
    void open(const std::string& uri, std::string text, int64_t version, std::string languageId);
    void close(const std::string& uri);
    bool contains(const std::string& uri) const;
    std::vector<std::string> openUris() const;

    // Full-document replace: `textDocumentSync: 1`, or an incremental
    // (`textDocumentSync: 2`) change event that omits `range` -- per the
    // LSP spec that also means "replace the whole document", so both sync
    // modes fold into this one path.
    void replaceText(const std::string& uri, std::string newText, int64_t version);

    // Ranged edit for `textDocumentSync: 2`. `range` is expressed in
    // `encoding` units against the document's text as it stands *right
    // now* (i.e. after any earlier edits already applied from the same
    // didChange's contentChanges array) -- apply a batch one at a time, in
    // order, exactly as the array arrived.
    void applyRangedEdit(const std::string& uri, const LspRange& range, const std::string& newText,
                          int64_t version, PositionEncodingKind encoding);

    struct TextSnapshot {
        std::string text;
        int64_t version = -1;
    };
    // Copies out the current text+version atomically. The compiler and any
    // PositionMapper built for a request should work off this snapshot, not
    // a live reference into the store, so a concurrent edit from the I/O
    // thread can never be observed mid-request.
    std::optional<TextSnapshot> getText(const std::string& uri) const;

    // Stores the analysis computed for `version`. No-ops if the document is
    // no longer open, or if a newer analysis is already stored -- keeps
    // `analyzedVersion` monotonic even if, hypothetically, analyses ever
    // completed out of order.
    void setAnalysis(const std::string& uri, int64_t version, std::shared_ptr<const DocumentAnalysis> analysis);

    struct AnalysisSnapshot {
        std::shared_ptr<const DocumentAnalysis> analysis;
        int64_t analyzedVersion = -1;
        int64_t textVersion = -1; // the document's CURRENT text version, which may be newer than analyzedVersion
    };
    std::optional<AnalysisSnapshot> getAnalysis(const std::string& uri) const;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, OpenDocument> m_docs;
};
