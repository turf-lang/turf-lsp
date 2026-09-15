#pragma once

#include <istream>
#include <optional>
#include <string>

// Reads one `Content-Length`-framed JSON-RPC message off a byte stream (LSP
// base protocol). Pulled out of main.cpp's read loop so it can
// actually be correct: the original inlined version assumed
// `Content-Length:` was the only header and the first line, used
// `std::getline` (which silently breaks if a client sends bare `\n` instead
// of `\r\n`), and did a single `cin.read(...)` for the body trusting it to
// never short-read. All three are real client behaviors this needs to
// tolerate, not just malformed input.
namespace LSPFramer {

// Reads headers until the blank line, then exactly Content-Length body
// bytes. Returns nullopt at a clean EOF (stream closed with nothing more to
// read) or if the stream errors/desyncs beyond recovery. Never throws.
std::optional<std::string> readMessage(std::istream& in);

} // namespace LSPFramer
