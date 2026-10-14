# Architecture of `turf-lsp`

`turf-lsp` is a C++20 language server. It runs the compiler's front end
in-process (the compiler's `turfcore` library) and answers editors over LSP 3.17
on stdio. What follows is how the pieces fit.

## Layers

```
 editor ── JSON-RPC over stdio ──►  LSPFramer      Content-Length framing
                                    LSPHandler     the protocol: dispatch, threads, JSON
                                       │
            ┌──────────────────────────┼───────────────────────────────┐
            ▼                          ▼                               ▼
     CompilerService            SemanticQuery                    TextFeatures, Uri, ...
     one analysis through       questions about an analysis:     functions of text alone:
     turf::analyze()            completion, hover, tokens,       diffs, folding, imports,
            │                   signature help, hints            declarations, file URIs
            ▼
     the compiler's query facade (include/turf/Analysis.h)
```

## One analysis

`CompilerService::analyze()` is one call to `turf::analyze()`. The compiler's
lexer, parser, resolver and type checker keep their state in process-wide
globals; the facade is the single place that saves, isolates and restores all of
it (the compiler's `scripts/check/check_boundary.sh` enforces that nothing else does).
The call is given:

- the document's text, as the editor has it (not the file on disk);
- the other open documents, which replace their files when an import loads them;
- the file's **package graph** (`ProjectGraph`: the server asks `turf graph` for
  it, once per project, and again only when `turf.toml` or `turf.lock` change),
  so `import` resolves as in `turf build`;
- a cancellation callback, asked at the two boundaries of the pipeline, so an
  analysis of text a later keystroke replaced stops early;
- the lint option, so the warnings `turf` prints reach the editor;
- the flow-analysis option, so what a compile finds while generating code
  (missing return, use before assignment, double free and use after free,
  unused variables, ...) reaches it too.

The result owns the program, the resolver's symbol table and the inferred types,
is immutable, and is kept per document so requests can query it after the fact.

## Threads

Two: the I/O thread frames and parses messages and does bookkeeping (document
text, cancellation flags); anything that touches the compiler runs on one worker
thread, because module resolution and walking an analysis's AST are outside the
facade's own lock. Diagnostics are debounced (~200 ms after the last edit).
`$/cancelRequest` sets a flag the worker checks between steps.

## Where the standard library comes from

`StdlibLocator` picks the installation root before any compiler code runs:
`$TURF_HOME` if it really contains `lib/std/core`, else `<root>/bin/..`, the
binary's own directory, or `~/.turf`. The winner is exported as `TURF_HOME` for
the compiler. A wrong `TURF_HOME` is reported and skipped; no library at all
leaves the server running, with a message to the editor and `E027` on imports
(the facade turns the missing prelude into a diagnostic rather than exiting).
The VS Code extension bundles `server/bin` and `server/lib/std` and starts the
server with `TURF_HOME` at that root.

## Text, positions, URIs

`PositionMapper` converts between the compiler's 1-based byte columns and LSP
positions in the negotiated encoding (UTF-16 by default, UTF-8 if the client
offers it). `Uri` converts `file://` URIs to and from paths, including Windows
drives and UNC. `TextFeatures` holds what needs no compiler and so also works on
a buffer that does not parse (folding, the minimal-diff formatting edits, import
statements, organize-imports, declaration scanning for workspace symbols).

## Tests

`build/turf-lsp-tests` (pure functions), `scripts/smoke.py` (the real server over
stdio, in a staged install with `TURF_HOME` unset), `scripts/check_grammar.py`
(the VS Code grammar against the compiler's reserved words), and the extension's
own unit and integration tests, which install the packaged `.vsix` into a real
VS Code. See the README.
