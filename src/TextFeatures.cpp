#include "TextFeatures.hpp"

#include <algorithm>
#include <cstdint>
#include <set>

namespace turf_lsp {

namespace {

// The text's lines, each keeping its own terminator ("\n" or "\r\n"); only the
// last may have none. Concatenating them gives the text back, so a line's
// byte offset is the sum of the lengths before it.
std::vector<std::string> linesWithEol(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        size_t end = nl == std::string::npos ? text.size() : nl + 1;
        lines.push_back(text.substr(start, end - start));
        start = end;
    }
    return lines;
}

bool isIdentChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

std::string trimLeft(const std::string& s) {
    size_t i = s.find_first_not_of(" \t");
    return i == std::string::npos ? std::string() : s.substr(i);
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// A line without its terminator.
std::string withoutEol(const std::string& line) {
    size_t n = line.size();
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) --n;
    return line.substr(0, n);
}

} // namespace

std::vector<TextChange> diffLines(const std::string& before, const std::string& after) {
    if (before == after) return {};
    const std::vector<std::string> a = linesWithEol(before), b = linesWithEol(after);

    std::vector<size_t> offset(a.size() + 1, 0); // byte offset of each old line
    for (size_t i = 0; i < a.size(); ++i) offset[i + 1] = offset[i] + a[i].size();

    size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;
    size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
        ++suffix;

    const size_t n = a.size() - prefix - suffix, m = b.size() - prefix - suffix; // the differing middle
    auto joined = [&](size_t from, size_t to) {
        std::string out;
        for (size_t j = from; j < to; ++j) out += b[j];
        return out;
    };

    // A rewrite too large for the table below is one change, which is correct
    // if less tidy.
    if (n * m > 4'000'000) return {{offset[prefix], offset[a.size() - suffix], joined(prefix, b.size() - suffix)}};

    // lcs[i][j]: the longest common subsequence of a[prefix+i..] and b[prefix+j..].
    // (n*m <= 4M bounds min(n,m) <= 2000, so a count fits 16 bits.)
    const size_t width = m + 1;
    std::vector<uint16_t> lcs((n + 1) * width, 0);
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            lcs[i * width + j] = a[prefix + i] == b[prefix + j]
                                     ? static_cast<uint16_t>(lcs[(i + 1) * width + j + 1] + 1)
                                     : std::max(lcs[(i + 1) * width + j], lcs[i * width + j + 1]);

    std::vector<TextChange> changes;
    bool open = false;
    size_t runOldStart = 0;     // old line index where the current run began
    std::string runNew;
    auto close = [&](size_t oldEnd) {
        if (!open) return;
        changes.push_back({offset[runOldStart], offset[oldEnd], runNew});
        open = false;
        runNew.clear();
    };
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && a[prefix + i] == b[prefix + j]) {
            close(prefix + i);
            ++i; ++j;
            continue;
        }
        if (!open) { open = true; runOldStart = prefix + i; }
        if (j < m && (i == n || lcs[i * width + j + 1] >= lcs[(i + 1) * width + j])) {
            runNew += b[prefix + j];
            ++j;
        } else {
            ++i; // an old line removed (replaced, if the run also inserts)
        }
    }
    close(prefix + n);
    return changes;
}

std::vector<FoldingRegion> foldingRegions(const std::string& text) {
    std::vector<FoldingRegion> out;
    struct Open { int line; };
    std::vector<Open> blocks;

    int line = 0;
    bool lineHasCode = false; // a non-space character other than a closing brace came before this one on the line
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\n') { ++line; lineHasCode = false; continue; }
        if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i + 1 < text.size() && text[i + 1] != '\n') ++i;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            lineHasCode = true;
            ++i;
            while (i < text.size() && text[i] != ch && text[i] != '\n') { if (text[i] == '\\') ++i; ++i; }
            if (i < text.size() && text[i] == '\n') { ++line; lineHasCode = false; }
            continue;
        }
        if (ch == '{') { blocks.push_back({line}); lineHasCode = true; continue; }
        if (ch == '}') {
            if (!blocks.empty()) {
                const int start = blocks.back().line;
                blocks.pop_back();
                // The closing brace stays visible when it begins its line.
                const int end = lineHasCode ? line : line - 1;
                if (end > start) out.push_back({start, end, ""});
            }
            lineHasCode = true;
            continue;
        }
        if (ch != ' ' && ch != '\t' && ch != '\r') lineHasCode = true;
    }

    // Runs of comment lines and of import lines.
    const std::vector<std::string> lines = linesWithEol(text);
    auto run = [&](bool (*pred)(const std::string&), const char* kind) {
        for (size_t i = 0; i < lines.size();) {
            if (!pred(trimLeft(lines[i]))) { ++i; continue; }
            size_t j = i;
            while (j + 1 < lines.size() && pred(trimLeft(lines[j + 1]))) ++j;
            if (j > i) out.push_back({static_cast<int>(i), static_cast<int>(j), kind});
            i = j + 1;
        }
    };
    run([](const std::string& l) { return startsWith(l, "//"); }, "comment");
    run([](const std::string& l) { return startsWith(l, "import "); }, "imports");

    std::sort(out.begin(), out.end(), [](const FoldingRegion& x, const FoldingRegion& y) {
        return x.startLine != y.startLine ? x.startLine < y.startLine : x.endLine > y.endLine;
    });
    return out;
}

std::vector<ImportStatement> importStatements(const std::string& text) {
    std::vector<ImportStatement> out;
    const std::vector<std::string> lines = linesWithEol(text);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!startsWith(lines[i], "import ")) continue;
        size_t pos = 7;
        while (pos < lines[i].size() && (lines[i][pos] == ' ' || lines[i][pos] == '\t')) ++pos;
        size_t end = pos;
        while (end < lines[i].size() && isIdentChar(lines[i][end])) ++end;
        if (end == pos) continue; // `import ` and nothing yet
        out.push_back({static_cast<int>(i), static_cast<int>(pos), static_cast<int>(end - pos), lines[i].substr(pos, end - pos)});
    }
    return out;
}

std::optional<TextChange> organizeImports(const std::string& text) {
    const std::vector<std::string> lines = linesWithEol(text);
    size_t first = 0;
    while (first < lines.size() && !startsWith(lines[first], "import ")) ++first;
    if (first == lines.size()) return std::nullopt;
    size_t last = first;
    while (last + 1 < lines.size() && startsWith(lines[last + 1], "import ")) ++last;

    std::set<std::string> sorted; // sorted and de-duplicated
    std::vector<std::string> original;
    for (size_t i = first; i <= last; ++i) {
        std::string l = withoutEol(lines[i]);
        while (!l.empty() && l.back() == ' ') l.pop_back();
        original.push_back(l);
        sorted.insert(l);
    }
    if (std::vector<std::string>(sorted.begin(), sorted.end()) == original) return std::nullopt;

    // Keep the file's own line ending, and the lack of one after the very
    // last line of a file that has none.
    const std::string& lastLine = lines[last];
    const std::string eol = lastLine.size() >= 2 && lastLine.compare(lastLine.size() - 2, 2, "\r\n") == 0 ? "\r\n" : "\n";
    const bool endsWithEol = !lastLine.empty() && lastLine.back() == '\n';

    size_t start = 0;
    for (size_t i = 0; i < first; ++i) start += lines[i].size();
    size_t end = start;
    for (size_t i = first; i <= last; ++i) end += lines[i].size();

    std::string replacement;
    size_t k = 0;
    for (const auto& l : sorted) {
        replacement += l;
        if (++k < sorted.size() || endsWithEol) replacement += eol;
    }
    return TextChange{start, end, replacement};
}

} // namespace turf_lsp
