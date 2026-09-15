#pragma once

#include <cstddef>
#include <string>
#include <vector>

// LSP positions are (line, character), 0-indexed, with `character` an
// UTF-16 **code-unit** offset by default (negotiable to UTF-8/UTF-32 via
// `general.positionEncodings` in `initialize`). The compiler's
// `SourceLocation` is 1-indexed lines and a **byte** column -- `CurCol` in
// Lexer.cpp advances once per `SourceFile->get()`, i.e. once per UTF-8
// *byte*, not per codepoint. Every boundary crossing between the two must
// go through here; no handler should do raw `col - 1` / `col + 1`
// arithmetic against compiler locations directly anymore, since that's
// silently wrong the moment a line contains anything outside ASCII (an
// accented identifier in a string/comment, a smart quote a user pasted,
// emoji, ...). A one-byte-one-column assumption only happens to work for
// pure-ASCII lines, which is most real code most of the time -- exactly the
// kind of bug that survives testing until it doesn't.
enum class PositionEncodingKind {
    UTF16, // LSP's mandatory default.
    UTF8,  // Negotiable; numerically identical to the compiler's byte column,
           // so conversion becomes a no-op copy. Neovim 0.10+ supports this.
    UTF32  // Negotiable; one unit per Unicode codepoint. Rare in practice.
};

std::string positionEncodingWireName(PositionEncodingKind kind);

// Picks the best encoding the client advertised in
// `capabilities.general.positionEncodings`, preferring utf-8 (cheapest to
// convert) if offered, otherwise falling back to the LSP-mandated utf-16
// default (also what an empty/absent list means).
PositionEncodingKind negotiatePositionEncoding(const std::vector<std::string>& clientSupported);

struct LspPosition {
    int line;      // 0-indexed
    int character; // 0-indexed, unit per the negotiated encoding
};

struct LspRange {
    LspPosition start;
    LspPosition end;
};

// One instance per document *snapshot* (rebuild whenever the text changes --
// it's just a byte-offset index over `text`, cheap to recompute and never
// safe to reuse across an edit).
class PositionMapper {
public:
    PositionMapper(const std::string& text, PositionEncodingKind encoding);

    // (line, character) in the negotiated encoding -> byte offset into
    // `text`. Clamps out-of-range input to the nearest valid offset rather
    // than throwing -- LSP positions can legally point one past the end of
    // a line/document (e.g. an insert at EOF), and a slightly-stale client
    // position racing a fast edit should degrade gracefully, not crash the
    // server.
    size_t toByteOffset(LspPosition p) const;

    // byte offset into `text` -> (line, character) in the negotiated
    // encoding.
    LspPosition toLspPosition(size_t byteOffset) const;

    // (line, character) -> the compiler's 1-indexed line + 1-indexed byte
    // column, i.e. what every `SourceLocation`/`ResolverSourceLocation` in
    // the compiler expects.
    void toCompilerLineCol(LspPosition p, int& outLine, int& outCol) const;

    // The compiler's 1-indexed line + 1-indexed byte column -> LSP
    // (line, character) in the negotiated encoding, spanning `byteLen`
    // bytes for the end position (byteLen=0 for a zero-width point).
    LspRange toLspRange(int compilerLine, int compilerCol, int byteLen) const;

    PositionEncodingKind encoding() const { return m_encoding; }

    // Byte offset of the start of a given 0-indexed line (clamped to
    // `text.size()` past the last line). Exposed for callers that need to
    // work with raw byte offsets within a specific line directly (e.g.
    // completion's textual trigger classification, which slices the buffer
    // by byte range rather than going through toLspPosition/toByteOffset).
    size_t lineStart(int line) const { return lineToByteOffset(line); }

private:
    const std::string& m_text;
    PositionEncodingKind m_encoding;
    std::vector<size_t> m_lineStarts; // byte offset of the start of each 0-indexed line

    size_t lineToByteOffset(int line) const;
    // Converts a character-unit column within a single line's bytes to a
    // byte offset, and back. `lineStart`/`lineEndExclusive` bound the line
    // in `m_text`.
    size_t unitColToByteOffset(size_t lineStart, size_t lineEndExclusive, int unitCol) const;
    int byteOffsetToUnitCol(size_t lineStart, size_t byteOffsetInLine) const;
};

// Computes the byte offset of the start of each line in `text` (line 0 at
// offset 0). Shared by PositionMapper and DocumentStore (which needs it to
// apply incremental edits without constructing a full PositionMapper for
// every intermediate step).
std::vector<size_t> computeLineStarts(const std::string& text);
