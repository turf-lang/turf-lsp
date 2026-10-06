// Unit tests for the parts of turf-lsp that are pure functions of their input:
// text features, the stdlib/project locators, and the text-only half of
// SemanticQuery. They need no compiler run and no editor, so they run in
// milliseconds and on every platform the server builds on -- including the
// Windows and macOS path shapes, which scripts/smoke.py (a Linux process
// driving the real server) cannot exercise.
//
//   build/turf-lsp-tests        exit 0 when every check passes

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "ProjectGraph.hpp"
#include "SemanticQuery.hpp"
#include "StdlibLocator.hpp"
#include "TextFeatures.hpp"

namespace fs = std::filesystem;

static int g_failed = 0;
static const char* g_test = "";

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            ++g_failed;                                                                        \
            std::cerr << "FAIL [" << g_test << "] " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        }                                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                                         \
    do {                                                                                       \
        auto&& va = (a);                                                                       \
        auto&& vb = (b);                                                                       \
        if (!(va == vb)) {                                                                     \
            ++g_failed;                                                                        \
            std::cerr << "FAIL [" << g_test << "] " << __FILE__ << ":" << __LINE__ << ": " #a " == " #b "\n" \
                      << "   got:  " << va << "\n   want: " << vb << "\n";                      \
        }                                                                                      \
    } while (0)

// A scratch directory, removed on exit.
struct Scratch {
    fs::path root;
    Scratch() {
        std::random_device rd;
        root = fs::temp_directory_path() / ("turf-lsp-unit-" + std::to_string(rd()));
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path write(const std::string& rel, const std::string& text = "") const {
        fs::path p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }
};

static std::string applyChanges(const std::string& text, std::vector<turf_lsp::TextChange> changes) {
    std::sort(changes.begin(), changes.end(), [](auto& a, auto& b) { return a.startByte > b.startByte; });
    std::string out = text;
    for (const auto& c : changes) out.replace(c.startByte, c.endByte - c.startByte, c.newText);
    return out;
}

// ── diffLines ───────────────────────────────────────────────────────────────

static void testDiffLines() {
    g_test = "diffLines";
    using namespace turf_lsp;
    CHECK(diffLines("a\nb\n", "a\nb\n").empty());

    // The changes, applied, always give the new text -- for every kind of edit.
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"a\nb\nc\n", "a\nB\nc\n"},                    // one line changed
        {"a\nb\nc\n", "a\nc\n"},                       // a line removed
        {"a\nc\n", "a\nb\nc\n"},                       // a line added
        {"a\n\n\n\nb\n", "a\n\nb\n"},                  // blank run collapsed
        {"fn f() {\nx;\n}\n", "fn f() {\n    x;\n}\n"}, // re-indent
        {"a\nb", "a\nb\n"},                            // final newline added
        {"a\nb\n", "a\nb"},                            // final newline removed
        {"a\r\nb\r\n", "a\r\n  b\r\n"},                // CRLF kept
        {"", "a\n"},
        {"a\n", ""},
        {"x\ny\nz\n", "1\n2\n3\n"},                    // nothing in common
    };
    for (const auto& [before, after] : cases)
        CHECK_EQ(applyChanges(before, diffLines(before, after)), after);

    // Minimal: one indented line is one change touching just that line.
    auto one = diffLines("a\nb\nc\nd\n", "a\n  b\nc\nd\n");
    CHECK_EQ(one.size(), size_t(1));
    CHECK_EQ(one[0].startByte, size_t(2));
    CHECK_EQ(one[0].endByte, size_t(4));
    CHECK_EQ(one[0].newText, std::string("  b\n"));

    // Two separate edits stay two edits (the unchanged middle is untouched).
    auto two = diffLines("a\nb\nc\nd\ne\n", "A\nb\nc\nd\nE\n");
    CHECK_EQ(two.size(), size_t(2));

    // A large rewrite still produces a correct (single) change.
    std::string big1, big2;
    for (int i = 0; i < 2500; ++i) { big1 += "line " + std::to_string(i) + "\n"; big2 += "other " + std::to_string(i) + "\n"; }
    CHECK_EQ(applyChanges(big1, diffLines(big1, big2)), big2);
}

// ── foldingRegions ──────────────────────────────────────────────────────────

static void testFolding() {
    g_test = "foldingRegions";
    using namespace turf_lsp;
    const std::string src =
        "// one\n"                   // 0
        "// two\n"                   // 1
        "import io;\n"               // 2
        "import json;\n"             // 3
        "\n"                         // 4
        "fn main(): int {\n"         // 5
        "    if (x) {\n"             // 6
        "        y;\n"               // 7
        "    }\n"                    // 8
        "    return \"}{\";\n"       // 9  (braces in a string)
        "}\n"                        // 10
        "fn one(): int { return 0; }\n"; // 11 (single line: no region)
    auto r = foldingRegions(src);
    auto has = [&](int s, int e, const char* kind) {
        for (const auto& x : r) if (x.startLine == s && x.endLine == e && x.kind == kind) return true;
        return false;
    };
    CHECK(has(0, 1, "comment"));
    CHECK(has(2, 3, "imports"));
    CHECK(has(5, 9, ""));   // the closing brace on line 10 stays visible
    CHECK(has(6, 7, ""));
    CHECK(r.size() == 4);
    // A comment block's own braces and a lone brace line are ignored properly.
    CHECK(foldingRegions("// { not a block\nfn f() {}\n").empty());
    CHECK(foldingRegions("").empty());
}

// ── importStatements / organizeImports ──────────────────────────────────────

static void testImports() {
    g_test = "imports";
    using namespace turf_lsp;
    auto imports = importStatements("// header\nimport io;\nimport net::[a, b];\nimport  json ;\nfn main() {}\nimport \n");
    CHECK_EQ(imports.size(), size_t(3));
    CHECK_EQ(imports[0].module, std::string("io"));
    CHECK_EQ(imports[0].line, 1);
    CHECK_EQ(imports[0].nameStart, 7);
    CHECK_EQ(imports[1].module, std::string("net"));
    CHECK_EQ(imports[2].module, std::string("json"));
    CHECK_EQ(imports[2].nameStart, 8);

    // Sorted and de-duplicated; the rest of the file untouched.
    const std::string messy = "// h\nimport net;\nimport io;\nimport io;\n\nfn main() {}\n";
    auto change = organizeImports(messy);
    CHECK(change.has_value());
    CHECK_EQ(applyChanges(messy, {*change}), std::string("// h\nimport io;\nimport net;\n\nfn main() {}\n"));
    CHECK(!organizeImports("import io;\nimport net;\n").has_value());   // already in order
    CHECK(!organizeImports("fn main() {}\n").has_value());              // none
    // CRLF and a file with no final newline.
    CHECK_EQ(applyChanges("import b;\r\nimport a;\r\n", {*organizeImports("import b;\r\nimport a;\r\n")}),
             std::string("import a;\r\nimport b;\r\n"));
    CHECK_EQ(applyChanges("import b;\nimport a;", {*organizeImports("import b;\nimport a;")}), std::string("import a;\nimport b;"));
}

// ── SemanticQuery's text-only parts ─────────────────────────────────────────

static void testRegions() {
    g_test = "regionAt";
    using R = CompletionRegion;
    auto at = [](const std::string& text, const char* marker) {
        return SemanticQuery::regionAt(text, text.find(marker));
    };
    CHECK(at("import io;\n\n@\n", "@") == R::TopLevel);
    CHECK(at("fn main(): int {\n    @\n}\n", "@") == R::Statement);
    CHECK(at("fn main(): int {\n    if (a) {\n        @\n    }\n}\n", "@") == R::Statement);
    CHECK(at("struct P {\n    x: @\n}\n", "@") == R::TypeBody);
    CHECK(at("pub struct P {\n    x: int,\n    @\n}\n", "@") == R::TypeBody);
    CHECK(at("enum E { A, @ }\n", "@") == R::TypeBody);
    CHECK(at("fn f(): int { return 0; }\n@\n", "@") == R::TopLevel);                 // closed
    CHECK(at("fn f(): int { return \"{\"; }\n@\n", "@") == R::TopLevel);             // brace in a string
    CHECK(at("fn f(): int { // {\n    @\n}\n", "@") == R::Statement);                // brace in a comment
    CHECK(at("struct P { x: int }\nfn f(): int {\n    @\n}\n", "@") == R::Statement); // after a closed struct
    CHECK(at("fn f(): int {\n    struct L {\n        a: @\n    }\n}\n", "@") == R::TypeBody); // local struct
}

static void testImportInsertion() {
    g_test = "importInsertion";
    auto result = [](const std::string& text, const char* module) {
        auto at = SemanticQuery::importInsertion(text, module);
        std::vector<turf_lsp::TextChange> ch;
        // Convert (line, character) to a byte offset.
        size_t off = 0;
        for (int l = 0; l < at.line; ++l) off = text.find('\n', off) + 1;
        off += at.character;
        ch.push_back({off, off, at.text});
        return applyChanges(text, ch);
    };
    CHECK_EQ(result("import io;\nfn main() {}\n", "json"), std::string("import io;\nimport json;\nfn main() {}\n"));
    CHECK_EQ(result("import io;\nimport net;\n\nfn main() {}\n", "json"), std::string("import io;\nimport net;\nimport json;\n\nfn main() {}\n"));
    CHECK_EQ(result("import io;", "json"), std::string("import io;\nimport json;"));          // no final newline
    CHECK_EQ(result("fn main() {}\n", "io"), std::string("import io;\n\nfn main() {}\n"));
    CHECK_EQ(result("// header\n\nfn main() {}\n", "io"), std::string("// header\n\nimport io;\n\nfn main() {}\n"));
    CHECK_EQ(result("unsafe module;\nfn main() {}\n", "io"), std::string("unsafe module;\nimport io;\n\nfn main() {}\n"));
    CHECK_EQ(result("", "io"), std::string("import io;\n\n"));
}

// ── Locating the stdlib and the project ─────────────────────────────────────

static void testStdlibLocator() {
    g_test = "StdlibLocator";
    using namespace turf_lsp;
    Scratch s;
    s.write("good/lib/std/core/int.tr");
    s.write("exe/bin/turf-lsp");
    s.write("exe/lib/std/core/int.tr");
    s.write("flat/turf-lsp");
    s.write("flat/lib/std/core/int.tr");
    s.write("home/.turf/lib/std/core/int.tr");
    s.write("empty/lib/std/README");   // lib/std without core/: not a stdlib

    CHECK(hasStdlib(s.root / "good"));
    CHECK(!hasStdlib(s.root / "empty"));
    CHECK(!hasStdlib(s.root / "nowhere"));

    auto locate = [&](StdlibEnv env) { return locateStdlib(env); };
    const std::string good = (s.root / "good").string();

    // TURF_HOME that is valid wins over the binary's own location.
    auto a = locate({good, (s.root / "exe/bin/turf-lsp").string(), "", ""});
    CHECK_EQ(a.root, (s.root / "good").lexically_normal().string());
    CHECK_EQ(a.how, std::string("TURF_HOME"));
    CHECK(a.warnings.empty());

    // A TURF_HOME that is wrong is reported and skipped, never trusted.
    auto b = locate({(s.root / "empty").string(), (s.root / "exe/bin/turf-lsp").string(), "", ""});
    CHECK_EQ(b.root, (s.root / "exe").lexically_normal().string());
    CHECK_EQ(b.warnings.size(), size_t(1));
    CHECK(b.warnings[0].find("TURF_HOME") != std::string::npos);

    // <root>/bin/turf-lsp finds <root>/lib/std; so does <root>/turf-lsp.
    auto c = locate({"", (s.root / "exe/bin/turf-lsp").string(), "", ""});
    CHECK_EQ(c.root, (s.root / "exe").lexically_normal().string());
    auto d = locate({"", (s.root / "flat/turf-lsp").string(), "", ""});
    CHECK_EQ(d.root, (s.root / "flat").lexically_normal().string());

    // The per-user install is the last resort.
    auto e = locate({"", (s.root / "elsewhere/bin/turf-lsp").string(), (s.root / "home").string(), ""});
    CHECK_EQ(e.root, (s.root / "home/.turf").lexically_normal().string());

    // Nothing anywhere: not found, and every place looked at is listed.
    auto f = locate({"", (s.root / "elsewhere/bin/turf-lsp").string(), (s.root / "nohome").string(), ""});
    CHECK(!f.found());
    CHECK(f.searched.size() >= 3);
}

static void testProjectLocator() {
    g_test = "ProjectGraph";
    using namespace turf_lsp;
    Scratch s;
    s.write("proj/turf.toml", "[package]\n");
    s.write("proj/src/main.tr");
    s.write("proj/src/deep/er/x.tr");
    s.write("proj/examples/e.tr");
    s.write("loose/a.tr");

    CHECK_EQ(findProjectRoot((s.root / "proj/src/deep/er/x.tr").string()), (s.root / "proj").string());
    CHECK_EQ(findProjectRoot((s.root / "proj/src/main.tr").string()), (s.root / "proj").string());
    CHECK_EQ(findProjectRoot((s.root / "loose/a.tr").string()), std::string());

    const std::string src = (s.root / "proj/src").string();
    CHECK(isInside((s.root / "proj/src/main.tr").string(), src));
    CHECK(isInside((s.root / "proj/src/deep/er/x.tr").string(), src));
    CHECK(!isInside((s.root / "proj/examples/e.tr").string(), src));
    CHECK(!isInside(src, src));                                   // a directory is not inside itself
    CHECK(isInside((s.root / "proj/src/../src/main.tr").string(), src));    // `..` is resolved
    // A sibling whose name merely starts with the same letters is not inside.
    s.write("proj/src2/y.tr");
    CHECK(!isInside((s.root / "proj/src2/y.tr").string(), src));

    // With no turf binary known, a file is analysed on its own.
    ProjectGraphProvider none;
    auto g = none.forFile((s.root / "proj/src/main.tr").string());
    CHECK(g.graphFile.empty());
    CHECK_EQ(g.root, (s.root / "proj").string());
}

int main() {
    testDiffLines();
    testFolding();
    testImports();
    testRegions();
    testImportInsertion();
    testStdlibLocator();
    testProjectLocator();
    if (g_failed) {
        std::cerr << g_failed << " check(s) failed\n";
        return 1;
    }
    std::cout << "turf-lsp-tests: all checks passed\n";
    return 0;
}
