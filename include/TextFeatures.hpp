#pragma once

#include <optional>
#include <string>
#include <vector>

// Editor features that are a function of the document's text alone: nothing
// here runs the compiler, so each works on the half-typed, syntactically
// broken buffers an editor has most of the time, and each can be tested
// without a language server. (Everything that needs to know what a name means
// goes through the compiler's analysis instead; see SemanticQuery.)
namespace turf_lsp {

// A replacement of bytes [startByte, endByte) of the old text by `newText`.
struct TextChange {
    size_t startByte = 0;
    size_t endByte = 0;
    std::string newText;
};

// The smallest set of whole-line changes that turn `before` into `after`
// (longest common subsequence of lines; very large rewrites fall back to one
// change). A formatter that replaces the whole document moves the user's
// cursor and clutters the undo history; this lets it touch only the lines
// that differ. Empty when the texts are equal.
std::vector<TextChange> diffLines(const std::string& before, const std::string& after);

struct FoldingRegion {
    int startLine = 0; // 0-based; the line that stays visible
    int endLine = 0;   // 0-based, inclusive; the last line that folds away
    std::string kind;  // "", "comment" or "imports" (LSP FoldingRangeKind)
};

// Foldable regions: every multi-line `{ }` block (the closing brace stays
// visible, as editors expect), runs of two or more `//` comment lines, and
// runs of two or more `import` lines. Braces inside strings, character
// literals and comments are ignored.
std::vector<FoldingRegion> foldingRegions(const std::string& text);

struct ImportStatement {
    int line = 0;          // 0-based
    int nameStart = 0;     // byte column of the module name
    int nameLength = 0;
    std::string module;    // `io` in `import io;`, `net` in `import net::[a, b];`
};

// The top-level `import <module>...;` statements, found by line (an import
// starts a line). For document links: each module name can open its file.
std::vector<ImportStatement> importStatements(const std::string& text);

// The edit that sorts and de-duplicates the file's first contiguous block of
// `import` lines; nullopt when there is none or it is already in order.
std::optional<TextChange> organizeImports(const std::string& text);

} // namespace turf_lsp
