#include "CompilerService.hpp"
#include "turf/Analysis.h"
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

// The compiler's front end is built on process-wide globals; running it over
// a buffer, safely, alongside whatever else the process is doing, is what the
// compiler's query facade (turf::analyze(), include/turf/Analysis.h) exists
// for. This file only turns its answer into the shapes the LSP layer wants.

CompilerService::CompilerService() = default;

namespace {

// The text of the files diagnostics point into, for widening a one-byte
// location to the token at it. The analysed document comes from memory (it
// is the editor's unsaved buffer, which need not match the disk); any other
// file -- an imported module the error surfaced in -- is read from disk once
// per analysis. The compiler's own line cache (AllSourceLines) is not an
// option: the facade restores it, so after analyze() it holds nothing of this
// analysis, and every squiggle would silently shrink to one byte.
class LineSource {
public:
    LineSource(std::string entryPath, std::string_view entryText) : m_entryPath(std::move(entryPath)) {
        m_files.emplace(m_entryPath, split(std::string(entryText)));
    }

    // Null when the file cannot be read.
    const std::vector<std::string>* lines(const std::string& path) {
        auto it = m_files.find(path);
        if (it != m_files.end()) return &it->second;
        std::ifstream in(path, std::ios::binary);
        std::vector<std::string> lines;
        if (in) {
            std::stringstream buf;
            buf << in.rdbuf();
            lines = split(buf.str());
        }
        // An unreadable file is remembered as empty so it is not retried for
        // each diagnostic in it.
        return &m_files.emplace(path, std::move(lines)).first->second;
    }

private:
    static std::vector<std::string> split(const std::string& text) {
        std::vector<std::string> out;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) out.push_back(std::move(line));
        return out;
    }

    std::string m_entryPath;
    std::unordered_map<std::string, std::vector<std::string>> m_files;
};

// Widens a diagnostic's 1-byte point location to the span of the
// identifier/token it points at -- a whole-token squiggle
// reads far better than a caret. The compiler doesn't carry real end-
// columns yet, so this is a best-effort scan through the line's text, not a
// real lexer re-tokenization. Falls back to 1 byte whenever there's nothing
// identifier-shaped at the location (punctuation-only diagnostics, a
// hand-built {0,0} sentinel, or a file that could not be read).
int widenDiagTokenByteLen(LineSource& source, const std::string& filename, int line, int col) {
    const std::vector<std::string>* lines = source.lines(filename);
    if (!lines) return 1;
    if (line < 1 || static_cast<size_t>(line) > lines->size()) return 1;
    const std::string& lineText = (*lines)[static_cast<size_t>(line - 1)];
    if (col < 1 || static_cast<size_t>(col - 1) >= lineText.size()) return 1;

    size_t start = static_cast<size_t>(col - 1);
    auto isIdentChar = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
    if (!isIdentChar(static_cast<unsigned char>(lineText[start]))) return 1;

    size_t end = start;
    while (end < lineText.size() && isIdentChar(static_cast<unsigned char>(lineText[end]))) ++end;
    return static_cast<int>(end - start);
}

// Converts one facade diagnostic into the LSP-shape-agnostic intermediate
// LSPHandler needs to finish the job: resolves the diagnostic's own
// (possibly-imported-module) filename, widens its point location, and folds
// the hint into the message text (LSP has no separate "hint" field). The
// facade's text is already plain: no terminal color escapes to strip.
MappedDiagnostic mapDiagnostic(const turf::Diagnostic& diag, const std::string& analyzedFilename,
                               LineSource& source) {
    MappedDiagnostic md;
    md.loc.filename = !diag.Where.Path.empty() ? diag.Where.Path : analyzedFilename;
    md.loc.line = static_cast<int>(diag.Where.Line);
    md.loc.col = static_cast<int>(diag.Where.Col);
    md.loc.byteLen = widenDiagTokenByteLen(source, md.loc.filename, md.loc.line, md.loc.col);
    md.isWarning = diag.Level == turf::Severity::Warning;
    md.code = diag.Code;
    md.message = diag.Message;
    if (!diag.Hint.empty()) md.message += "\nhint: " + diag.Hint;

    for (const auto& rel : diag.Related) {
        MappedRelatedInfo mri;
        mri.loc.filename = !rel.Where.Path.empty() ? rel.Where.Path : md.loc.filename;
        mri.loc.line = static_cast<int>(rel.Where.Line);
        mri.loc.col = static_cast<int>(rel.Where.Col);
        mri.loc.byteLen = widenDiagTokenByteLen(source, mri.loc.filename, mri.loc.line, mri.loc.col);
        mri.description = rel.Message;
        md.related.push_back(std::move(mri));
    }
    return md;
}

} // namespace

std::string CompilerService::resolveModuleFile(const std::string& module, const std::string& importingFile) {
    turf::AnalysisOptions options;
    options.PackageGraphFile = m_projects.forFile(importingFile).graphFile;
    return turf::resolveModuleFile(module, importingFile, options);
}

std::unique_ptr<DocumentAnalysis> CompilerService::analyze(std::string_view source, std::string_view filename,
                                                             const std::function<bool()>& isCancelled) {
    // The facade runs the lexer, parser, resolver and typechecker over the
    // buffer (never codegen, never `--smart`: the model is slow and
    // nondeterministic, wrong for a pass that reruns on every debounced
    // keystroke), keeps going past errors so one bad function leaves the
    // rest usable, reports every diagnostic rather than one per line (a
    // second genuine error on an errored line must still get its own
    // squiggle), and leaves the process's compiler state exactly as it
    // found it. `isCancelled` is asked at the pipeline's phase boundaries.
    turf::AnalysisOptions options;
    options.ShouldCancel = isCancelled;
    // A real compile lints while generating code, which an analysis never
    // does; without this the editor would miss the warnings `turf` prints
    // (unused variables, unreachable code, shadowing).
    options.RunLint = true;
    // Likewise the flow checks a real compile runs during code generation:
    // missing return, use before assignment, double free, use after free,
    // unchecked nullable references, unused variables and the rest.
    options.RunFlowAnalysis = true;

    // A file in a project is analysed with the project's package graph, so
    // `import` finds its sibling modules and dependencies as `turf build`
    // does. A graph that cannot be made is reported on the file (a manifest
    // that does not parse is the user's to fix) and the file is analysed on
    // its own.
    const turf_lsp::ProjectGraph project = m_projects.forFile(std::string(filename));
    options.PackageGraphFile = project.graphFile;

    // The analysed document first (the facade's entry file), then every
    // other open document: an import that loads one of them sees its unsaved
    // text.
    std::vector<turf::SourceBuffer> buffers;
    buffers.push_back({std::string(filename), std::string(source)});
    if (m_openDocuments) {
        for (auto& open : m_openDocuments())
            if (open.Path != filename) buffers.push_back(std::move(open));
    }

    turf::AnalysisResult result = turf::analyze(buffers, options);
    if (result.cancelled()) return nullptr;

    auto analysis = std::make_unique<DocumentAnalysis>(std::move(result));
    LineSource lines{std::string(filename), source};
    if (!project.problem.empty()) {
        MappedDiagnostic md;
        md.loc.filename = std::string(filename);
        md.loc.line = 1;
        md.loc.col = 1;
        md.message = project.problem;
        analysis->diagnostics.push_back(std::move(md));
    }
    for (const auto& diag : analysis->result.diagnostics())
        analysis->diagnostics.push_back(mapDiagnostic(diag, std::string(filename), lines));
    return analysis;
}
