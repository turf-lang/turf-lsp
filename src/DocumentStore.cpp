#include "DocumentStore.hpp"

void DocumentStore::open(const std::string& uri, std::string text, int64_t version, std::string languageId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    OpenDocument doc;
    doc.uri = uri;
    doc.text = std::move(text);
    doc.version = version;
    doc.languageId = std::move(languageId);
    m_docs[uri] = std::move(doc);
}

void DocumentStore::close(const std::string& uri) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_docs.erase(uri);
}

bool DocumentStore::contains(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_docs.find(uri) != m_docs.end();
}

std::vector<std::string> DocumentStore::openUris() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> uris;
    uris.reserve(m_docs.size());
    for (const auto& [uri, doc] : m_docs) uris.push_back(uri);
    return uris;
}

void DocumentStore::replaceText(const std::string& uri, std::string newText, int64_t version) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_docs.find(uri);
    if (it == m_docs.end()) return;
    it->second.text = std::move(newText);
    it->second.version = version;
}

void DocumentStore::applyRangedEdit(const std::string& uri, const LspRange& range, const std::string& newText,
                                     int64_t version, PositionEncodingKind encoding) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_docs.find(uri);
    if (it == m_docs.end()) return;

    // Byte offsets must be computed against the text as it stands BEFORE
    // this edit -- that's what `range` is expressed relative to.
    PositionMapper mapper(it->second.text, encoding);
    size_t startByte = mapper.toByteOffset(range.start);
    size_t endByte = mapper.toByteOffset(range.end);
    if (endByte < startByte) std::swap(startByte, endByte);

    it->second.text.replace(startByte, endByte - startByte, newText);
    it->second.version = version;
}

std::optional<DocumentStore::TextSnapshot> DocumentStore::getText(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_docs.find(uri);
    if (it == m_docs.end()) return std::nullopt;
    return TextSnapshot{it->second.text, it->second.version};
}

void DocumentStore::setAnalysis(const std::string& uri, int64_t version,
                                 std::shared_ptr<const DocumentAnalysis> analysis) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_docs.find(uri);
    if (it == m_docs.end()) return; // closed while analysis was in flight
    if (version < it->second.analyzedVersion) return; // superseded; keep the newer one
    it->second.analysis = std::move(analysis);
    it->second.analyzedVersion = version;
}

std::optional<DocumentStore::AnalysisSnapshot> DocumentStore::getAnalysis(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_docs.find(uri);
    if (it == m_docs.end()) return std::nullopt;
    AnalysisSnapshot snap;
    snap.analysis = it->second.analysis;
    snap.analyzedVersion = it->second.analyzedVersion;
    snap.textVersion = it->second.version;
    return snap;
}
