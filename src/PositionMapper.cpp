#include "PositionMapper.hpp"
#include <algorithm>
#include <cstdint>

namespace {

int utf8SequenceLength(unsigned char lead) {
    if ((lead & 0x80) == 0x00) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1; // invalid lead byte -- step by one raw byte so we never get stuck
}

// Decodes the codepoint starting at `pos` (< `end`). Writes the sequence's
// byte length to `outByteLen`. Tolerant of truncated/invalid UTF-8 (falls
// back to a 1-byte step) since a document buffer is user-typed text, not a
// validated wire format -- it must never be able to hang or overrun here.
uint32_t decodeCodepoint(const std::string& text, size_t pos, size_t end, int& outByteLen) {
    unsigned char lead = static_cast<unsigned char>(text[pos]);
    int len = utf8SequenceLength(lead);
    if (len == 1 || pos + static_cast<size_t>(len) > end) {
        outByteLen = 1;
        return lead;
    }
    uint32_t cp = 0;
    switch (len) {
        case 2: cp = lead & 0x1Fu; break;
        case 3: cp = lead & 0x0Fu; break;
        case 4: cp = lead & 0x07u; break;
    }
    for (int i = 1; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(text[pos + static_cast<size_t>(i)]);
        if ((c & 0xC0) != 0x80) {
            outByteLen = 1; // not a valid continuation byte; bail to a 1-byte step
            return lead;
        }
        cp = (cp << 6) | (c & 0x3Fu);
    }
    outByteLen = len;
    return cp;
}

int unitsForCodepoint(uint32_t cp, PositionEncodingKind encoding) {
    switch (encoding) {
        case PositionEncodingKind::UTF16: return cp > 0xFFFFu ? 2 : 1;
        case PositionEncodingKind::UTF32: return 1;
        case PositionEncodingKind::UTF8:  return 0; // handled by the byte-identity fast path
    }
    return 1;
}

} // namespace

std::string positionEncodingWireName(PositionEncodingKind kind) {
    switch (kind) {
        case PositionEncodingKind::UTF16: return "utf-16";
        case PositionEncodingKind::UTF8:  return "utf-8";
        case PositionEncodingKind::UTF32: return "utf-32";
    }
    return "utf-16";
}

PositionEncodingKind negotiatePositionEncoding(const std::vector<std::string>& clientSupported) {
    // Prefer utf-8: it makes every conversion below a byte-identity no-op,
    // matching the compiler's own byte columns exactly. Only take it if the
    // client actually listed it as supported (LSP requires the server to
    // pick from what the client advertised, not assume).
    if (std::find(clientSupported.begin(), clientSupported.end(), "utf-8") != clientSupported.end()) {
        return PositionEncodingKind::UTF8;
    }
    // utf-16 is the LSP-mandated default and always valid even if the
    // client's list is empty/absent.
    return PositionEncodingKind::UTF16;
}

std::vector<size_t> computeLineStarts(const std::string& text) {
    std::vector<size_t> starts;
    starts.push_back(0);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') starts.push_back(i + 1);
    }
    return starts;
}

PositionMapper::PositionMapper(const std::string& text, PositionEncodingKind encoding)
    : m_text(text), m_encoding(encoding), m_lineStarts(computeLineStarts(text)) {}

size_t PositionMapper::lineToByteOffset(int line) const {
    if (line < 0) return 0;
    if (static_cast<size_t>(line) >= m_lineStarts.size()) return m_text.size();
    return m_lineStarts[static_cast<size_t>(line)];
}

size_t PositionMapper::unitColToByteOffset(size_t lineStart, size_t lineEndExclusive, int unitCol) const {
    if (unitCol <= 0) return lineStart;

    if (m_encoding == PositionEncodingKind::UTF8) {
        size_t off = lineStart + static_cast<size_t>(unitCol);
        return std::min(off, lineEndExclusive);
    }

    int unitsSeen = 0;
    size_t pos = lineStart;
    while (pos < lineEndExclusive) {
        int byteLen = 1;
        uint32_t cp = decodeCodepoint(m_text, pos, lineEndExclusive, byteLen);
        int units = unitsForCodepoint(cp, m_encoding);
        if (unitsSeen + units > unitCol) break; // target falls inside this codepoint; stop before it
        unitsSeen += units;
        pos += static_cast<size_t>(byteLen);
        if (unitsSeen >= unitCol) break;
    }
    return pos;
}

int PositionMapper::byteOffsetToUnitCol(size_t lineStart, size_t byteOffsetInLine) const {
    if (m_encoding == PositionEncodingKind::UTF8) {
        return static_cast<int>(byteOffsetInLine - lineStart);
    }

    int unitsSeen = 0;
    size_t pos = lineStart;
    while (pos < byteOffsetInLine) {
        int byteLen = 1;
        uint32_t cp = decodeCodepoint(m_text, pos, byteOffsetInLine, byteLen);
        unitsSeen += unitsForCodepoint(cp, m_encoding);
        pos += static_cast<size_t>(byteLen);
    }
    return unitsSeen;
}

size_t PositionMapper::toByteOffset(LspPosition p) const {
    size_t lineStart = lineToByteOffset(p.line);
    size_t lineEnd = (static_cast<size_t>(p.line) + 1 < m_lineStarts.size())
                         ? m_lineStarts[static_cast<size_t>(p.line) + 1]
                         : m_text.size();
    // Exclude the line's own trailing newline from the "in-line" byte range
    // (a position can still legally target lineEnd itself, e.g. at EOF).
    if (lineEnd > lineStart && lineEnd <= m_text.size() && m_text[lineEnd - 1] == '\n') {
        lineEnd -= 1;
        if (lineEnd > lineStart && m_text[lineEnd - 1] == '\r') lineEnd -= 1;
    }
    return unitColToByteOffset(lineStart, lineEnd, p.character);
}

LspPosition PositionMapper::toLspPosition(size_t byteOffset) const {
    byteOffset = std::min(byteOffset, m_text.size());
    // Find the line containing byteOffset: last lineStart <= byteOffset.
    auto it = std::upper_bound(m_lineStarts.begin(), m_lineStarts.end(), byteOffset);
    size_t lineIdx = (it == m_lineStarts.begin()) ? 0 : static_cast<size_t>(it - m_lineStarts.begin() - 1);
    size_t lineStart = m_lineStarts[lineIdx];
    int character = byteOffsetToUnitCol(lineStart, byteOffset);
    return LspPosition{static_cast<int>(lineIdx), character};
}

void PositionMapper::toCompilerLineCol(LspPosition p, int& outLine, int& outCol) const {
    size_t lineStart = lineToByteOffset(p.line);
    size_t lineEnd = (static_cast<size_t>(p.line) + 1 < m_lineStarts.size())
                         ? m_lineStarts[static_cast<size_t>(p.line) + 1]
                         : m_text.size();
    if (lineEnd > lineStart && lineEnd <= m_text.size() && m_text[lineEnd - 1] == '\n') {
        lineEnd -= 1;
        if (lineEnd > lineStart && m_text[lineEnd - 1] == '\r') lineEnd -= 1;
    }
    size_t byteOffset = unitColToByteOffset(lineStart, lineEnd, p.character);
    // Compiler locations are 1-indexed for both line and (byte) column.
    outLine = p.line + 1;
    outCol = static_cast<int>(byteOffset - lineStart) + 1;
}

LspRange PositionMapper::toLspRange(int compilerLine, int compilerCol, int byteLen) const {
    int lspLine0 = compilerLine - 1;
    size_t lineStart = lineToByteOffset(lspLine0);
    size_t startByte = lineStart + static_cast<size_t>(compilerCol - 1);
    startByte = std::min(startByte, m_text.size());
    size_t endByte = std::min(startByte + static_cast<size_t>(std::max(byteLen, 0)), m_text.size());

    LspRange range;
    range.start = toLspPosition(startByte);
    range.end = toLspPosition(endByte);
    return range;
}
