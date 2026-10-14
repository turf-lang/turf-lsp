# Changelog

## 1.0.0

First release. The extension is self-contained: the language server and the
standard library are bundled, per platform, so there is nothing to install or
configure.

- Diagnostics (with lint warnings), completion (scope-aware, `module::` members,
  import names, keywords and templates, auto-import), signature help, inlay
  hints (parameter names), hover, go to definition, references, rename, document
  highlight, outline, workspace symbol search, document links, folding, semantic
  highlighting.
- Quick fixes: add a missing import, organize imports, prefix an unused variable.
- Formatting (whole document or selection), changing only the lines that differ.
- Projects: imports resolve through the project's package graph (`turf graph`):
  own modules under `src/`, path and git dependencies, `tests/`.
- Unsaved edits to one file are seen by files that import it.
- Run, Build, Check and Run Tests as tasks, commands and code lenses; tests in
  the Testing view. These need the Turf toolchain, and say so when it is missing.
- Workspace Trust: analysis works in untrusted workspaces; running code does not.

Minimum language-server version: 1.0.0.
