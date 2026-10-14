#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

#include "DocumentAnalysis.hpp"
#include "ProjectGraph.hpp"

class CompilerService {
public:
    CompilerService();
    ~CompilerService() = default;

    // Where `turf` is, so a file in a project is analysed with the project's
    // package graph (see ProjectGraph.hpp). Empty: every file is analysed on
    // its own. Worker thread only, like analyze().
    void setTurfExecutable(std::string turfExe) { m_projects.setTurfExecutable(std::move(turfExe)); }

    // The editor's open documents (path, current text). Given to every
    // analysis as extra buffers, so an import of a module the user is editing
    // sees the unsaved text, not the file on disk. Called on the worker
    // thread, once per analysis; must be thread-safe and quick.
    using OpenDocuments = std::function<std::vector<turf::SourceBuffer>()>;
    void setOpenDocuments(OpenDocuments provider) { m_openDocuments = std::move(provider); }

    // Must only ever be called from the single worker thread that owns the
    // compiler's global mutable state -- never from the I/O
    // thread, and never concurrently with another call to analyze().
    //
    // `isCancelled`, if provided, is checked at the two natural phase
    // boundaries the pipeline already has (after parsing, before
    // resolveAll(); after resolveAll(), before checkAll()) -- coarse
    // checkpoint granularity, not mid-pass interruption, since the
    // compiler's passes themselves have no interruption points and adding
    // them would be a much larger change than an LSP's responsiveness
    // needs. Returns nullptr if
    // cancelled before completion.
    std::unique_ptr<DocumentAnalysis> analyze(std::string_view source, std::string_view filename,
                                               const std::function<bool()>& isCancelled = {});

    // The file `import <module>;` loads when written in `importingFile`,
    // resolved the way analyze() resolves it for that file -- with the file's
    // project (its own modules, its dependencies) when it is in one. For
    // opening the file behind a module name (go to definition, hover
    // documentation). "" when the name resolves to no single file. Worker
    // thread only, like analyze().
    std::string resolveModuleFile(const std::string& module, const std::string& importingFile);

private:
    turf_lsp::ProjectGraphProvider m_projects;
    OpenDocuments m_openDocuments;
};
