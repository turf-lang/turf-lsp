#include "LSPFramer.hpp"
#include "Logger.hpp"
#include <cctype>

namespace {

// Reads one header line, stripping a trailing \r if present so both \r\n
// (spec-mandated) and bare \n (what some non-conformant clients/pipes
// actually send) work. Returns false on EOF with nothing read.
bool readLine(std::istream& in, std::string& out) {
    out.clear();
    int ch;
    bool sawAny = false;
    while ((ch = in.get()) != std::istream::traits_type::eof()) {
        sawAny = true;
        if (ch == '\n') {
            if (!out.empty() && out.back() == '\r') out.pop_back();
            return true;
        }
        out.push_back(static_cast<char>(ch));
    }
    return sawAny && !out.empty();
}

} // namespace

namespace LSPFramer {

std::optional<std::string> readMessage(std::istream& in) {
    long long contentLength = -1;

    // Parse every header until the blank line -- don't assume
    // Content-Length is first or the only one (Content-Type is legal and
    // some clients send it).
    for (;;) {
        std::string line;
        if (!readLine(in, line)) return std::nullopt; // clean EOF between messages
        if (line.empty()) break; // blank line ends the header block

        auto colon = line.find(':');
        if (colon == std::string::npos) continue; // ignore malformed header line

        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        // Trim leading spaces from the value.
        size_t start = value.find_first_not_of(' ');
        value = (start == std::string::npos) ? "" : value.substr(start);

        if (name == "Content-Length") {
            try {
                contentLength = std::stoll(value);
            } catch (const std::exception&) {
                Logger::warn("LSPFramer: malformed Content-Length header: '" + value + "'");
                return std::nullopt;
            }
        }
        // Any other header (e.g. Content-Type) is intentionally ignored.
    }

    if (contentLength < 0) {
        Logger::warn("LSPFramer: header block ended without a Content-Length");
        return std::nullopt;
    }

    std::string body;
    body.resize(static_cast<size_t>(contentLength));
    size_t total = 0;
    while (total < body.size()) {
        in.read(&body[total], static_cast<std::streamsize>(body.size() - total));
        std::streamsize got = in.gcount();
        if (got <= 0) {
            Logger::warn("LSPFramer: stream ended mid-body (expected " +
                         std::to_string(body.size()) + " bytes, got " + std::to_string(total) + ")");
            return std::nullopt;
        }
        total += static_cast<size_t>(got);
    }

    return body;
}

} // namespace LSPFramer
