# Architecture of turf-lsp

turf-lsp is a C++20 language server. It runs the compiler's front end
in-process (the `turfcore` library of [turf-lang](https://github.com/turf-lang/turf-lang))
and answers editors over LSP 3.17 on stdio. Answers come from the compiler's own
lexer, parser, resolver and type checker, so they cannot drift from what `turf`
accepts.

## Layers

```
 editor -- JSON-RPC over stdio -->  LSPFramer      Content-Length framing
                                    LSPHandler     the protocol: dispatch, threads, JSON
                                       |
            +--------------------------+-------------------------------+
            v                          v                               v
     CompilerService            SemanticQuery                    TextFeatures, Uri, ...
     one analysis through       questions about an analysis:     functions of text alone:
     turf::analyze()            completion, hover, tokens,       diffs, folding, imports,
            |                   signature help, hints            declarations, file URIs
            v
     the compiler's query facade (include/turf/Analysis.h)
```

| Source | Role |
|---|---|
| `LSPFramer`, `Logger` | message framing; logging to stderr (stdout is the protocol) |
| `LSPHandler` | dispatch, the two threads, request cancellation, every `handleX`/`doX` |
| `CompilerService` | one analysis through `turf::analyze()`; resolves a module name to its file |
| `SemanticQuery` | answers questions about an analysis |
| `DocumentStore` | open documents: text, version, the latest analysis |
| `ProjectGraph` | a project's packages, from `turf graph` |
| `WorkspaceIndex` | the `.tr` files under the workspace folders |
| `StdlibLocator` | finds the standard library |
| `PositionMapper`, `Uri`, `TextFeatures` | positions, `file://` URIs, and what needs no compiler |

## Analyses go through the query facade

The server runs the compiler only through `include/turf/Analysis.h`. The compiler's
lexer, parser, resolver and type checker keep their state in process-wide globals,
because it was written as a batch tool. The facade is the one place that saves,
isolates and restores all of it, and the compiler's `scripts/check/check_boundary.sh`
enforces that nothing else does. A change to that state then has one owner, and a
new compiler global cannot corrupt a later analysis unnoticed. `SemanticQuery` does
read the AST an analysis returns (it includes the compiler's AST headers for that),
but only after the facade has returned and only on the worker thread (see Threads).

## One analysis

`CompilerService::analyze()` is one call to `turf::analyze()`. It is given:

- the document's text, as the editor has it (not the file on disk);
- the other open documents, which replace their files when an import loads them;
- the file's **package graph** (`ProjectGraph`: the server asks `turf graph` for
  it, once per project, and again only when `turf.toml` or `turf.lock` change),
  so `import` resolves as in `turf build`. Without a `turf` executable every file
  is analysed alone;
- a cancellation callback, asked at the two boundaries of the pipeline (after
  parsing, and after resolving), so an analysis of text a later keystroke
  replaced stops early. The passes themselves have no interruption points;
- the lint option, so the warnings `turf` prints reach the editor;
- the flow-analysis option, so what a compile finds while generating code
  (missing return, use before assignment, double free and use after free, unused
  variables, ...) reaches it too.

The result owns the program, the resolver's symbol table and the inferred types.
It is immutable and is kept per document, so requests can query it after the fact.

## Threads and cancellation

Two threads do the work, and a third only keeps time.

- The **I/O thread** frames and parses messages and does bookkeeping: document
  text, cancellation flags, lifecycle state. It never waits on an analysis.
- One **worker thread** owns all compiler access. Two analyses cannot run
  concurrently, and module resolution and the AST walks in `SemanticQuery` are
  outside the facade's lock, so everything that touches the compiler is queued to
  this one thread. A mutex around a synchronous server would also serialise
  correctly but would block the read loop during a slow analysis, and then
  `didChange` and `$/cancelRequest` would wait behind the work they are meant to
  stop. Separating I/O from compute is what makes cancellation possible.
  Running analyses in parallel would need the compiler to carry a per-analysis
  context instead of globals, which is a compiler change, not a server one.
- A **debounce thread** turns a burst of `didChange` into one diagnostics run,
  200 ms after the last edit (`TURF_LSP_DEBOUNCE_MS` overrides it).

On-demand requests (hover, completion, definition, ...) do not wait for the
debounce. Each calls `ensureFreshAnalysis()` as it runs on the worker, which
re-analyses at once if the stored analysis is older than the document's text. A
request therefore never sees stale text, at the price of an extra analysis when
it lands in the middle of typing. `DocumentStore::setAnalysis()` only accepts a
newer version, so a slow older analysis cannot replace a newer one.

`$/cancelRequest` sets a per-request flag, checked when a handler starts, between
its steps and at the analysis boundaries above. Errors use the standard codes: a
cancelled request gets -32800, a request whose document closed or changed under it
-32801, an unknown method -32601, a request before `initialize` -32002, and an
exception inside a handler -32603. Every request gets exactly one response,
including when a handler throws.

Text sync is incremental (`change: 2`). `DocumentStore::applyRangedEdit()` converts
the range through a `PositionMapper` over the current text and splices; a change
without a range replaces the whole text, and a client may mix both in one batch.

## Completion on a half-typed buffer

At the cursor the buffer is almost never valid: `io::` or `p.` with nothing after
it does not parse. The compiler's recovery parser resynchronises only at the
statement boundaries of the top level, so left alone, one such line would drop
every local of the enclosing function from the analysis, and those are exactly the
names completion needs. So the server replaces the partial word with a placeholder
identifier and a terminator and runs a throwaway analysis of that text. The
throwaway is never stored: the document keeps its real text and its real analysis.

One trigger skips this. After `Name::` the base is a module, struct or enum, each
registered by its own top-level declaration, so the stored analysis answers it
whether or not the current statement parsed; the patched analysis is used only if
that comes back empty. A bare word and a `.` trigger always take the patched path,
because they usually name a local of the function being typed in. Widening the fast
path to "whenever the stored result is non-empty" does not work: bare-word results
are padded with keywords and types, so non-empty says nothing about the locals.

## References and rename

References are matched by identity, not by name: a declaration's location together
with the module file it belongs to, resolved from the file being scanned. Two
locals called `x` are different symbols, and the same function used from two files
is one. Workspace-wide references analyse every file `WorkspaceIndex` listed with
a fresh, separate analysis per file, and keep nothing between requests. That is
simple and correct and costs a scan per request, which is fine at the size of
typical projects; a persistent occurrence index is the next step if it stops being.
Rename is references plus text replacement. It refuses the standard library, a name
that is not a legal identifier, and reserved words. It does not check whether the
new name collides with another declaration visible in an affected scope.

Hover documentation is the contiguous run of `//` lines directly above a
declaration. A blank line between the comment and the declaration detaches it.

## Where the standard library comes from

`StdlibLocator` picks the installation root before any compiler code runs:
`$TURF_HOME` if it really contains `lib/std/core`, else `<root>/bin/..` or the
server's own directory, then `~/.turf` (`%LOCALAPPDATA%\turf` on Windows). The
winner is exported as `TURF_HOME` for the compiler. A `TURF_HOME` that is wrong is
reported and skipped; no library at all leaves the server running, with a message
to the editor and `E027` on imports (the facade turns the missing prelude into a
diagnostic instead of exiting). The VS Code extension bundles `server/bin` and
`server/lib/std` and starts the server with `TURF_HOME` at that root.

## Text, positions, URIs

`PositionMapper` converts between the compiler's 1-based byte columns and LSP
positions in the negotiated encoding (UTF-16 by default, UTF-8 if the client
offers it). Every handler converts through it; none does its own `col - 1`.
`Uri` converts `file://` URIs to and from paths, including Windows drives and UNC.
`TextFeatures` holds what needs no compiler and so also works on a buffer that
does not parse: folding, the minimal-diff formatting edits, import statements,
organize imports, and declaration scanning for workspace symbols.

Formatting calls the compiler's own formatter (`turf::format::formatChecked`, the
one behind `turf fmt`) on the text and returns only the lines that differ, so the
cursor and the undo history stay put. A result that changes more than whitespace is
dropped rather than applied.

## Known limitations

- Hover, definition, completion and semantic tokens ignore everything inside a
  `recover ... produce ...` or `recover as e run { ... }` expression: the visitors
  in `SemanticQuery.cpp` have no `visitRecoverExpr`, and the base visitor skips
  what it does not override.
- Rename does not detect name collisions (see above).
- No call or type hierarchy, no debug adapter, and no inferred-type inlay hints:
  Turf declarations spell their types.

## Tests

`build/turf-lsp-tests` tests the pure functions. `scripts/smoke.py` runs the real
server over stdio in a staged install with `TURF_HOME` unset. `scripts/check_grammar.py`
checks the VS Code grammar against the compiler's reserved words. The extension has
its own unit tests and an integration test that installs the packaged `.vsix` into
a real VS Code. [DEVELOPING.md](DEVELOPING.md) has the commands.
