# 11 — Editor clients (VS Code + forks, Neovim)

## Goal
Ship first-class clients for **VS Code and its forks** (Cursor, Windsurf,
VSCodium) and **Neovim**, plus documented config for other LSP editors. The
server is editor-agnostic; these are thin.

## Current state (consolidate first)
There are **three** overlapping trees — clean this up before adding features:
- `turf-lsp/editors/vscode/` — extension with `package.json`, `extension.ts`,
  `syntaxes/turf.tmLanguage.json`. Client points at a `turf-lsp` binary via the
  `turf.server.path` setting.
- `turf-lsp/vscode-turf/` — a **second**, near-duplicate VS Code extension
  (`out/`, `syntaxes/`, `language-configuration.json`, its own `extension.ts`).
- `turf-lang/grammars/` — unrelated (`slm_suggestion.gbnf`), plus the tmLanguage
  duplicated across the two extension dirs.

**Action:** pick `editors/vscode/` as the canonical extension; migrate anything
unique from `vscode-turf/` (notably `language-configuration.json` — brackets,
comments, auto-closing pairs) into it; **delete `vscode-turf/`**. Keep exactly
one `turf.tmLanguage.json` (shared source of truth, referenced by the extension;
doc 04).

## VS Code extension (`editors/vscode/`)

### Structure
- `package.json`:
  - `contributes.languages`: `turf`, extensions `[".tr"]`, aliases, an icon,
    and `configuration: ./language-configuration.json`.
  - `contributes.grammars`: map `source.turf` → `syntaxes/turf.tmLanguage.json`.
  - `contributes.configuration`: `turf.server.path` (have),
    `turf.home` (→ passed to server for `TURF_HOME`/module resolution),
    `turf.trace.server` (LSP tracing), `turf.diagnostics.enable`,
    `turf.format.onSave`, `turf.completion.snippets`.
  - `contributes.semanticTokenScopes` (optional): map our semantic token types to
    theme scopes for themes that don't map them by default.
  - `activationEvents`: `onLanguage:turf`.
  - `capabilities`: `untrustedWorkspaces` — since the server runs a native binary
    and executes analysis, declare workspace-trust behavior; don't auto-run the
    server in restricted mode without consent.
- `language-configuration.json`: line comment `//`, brackets/auto-close for
  `() [] {} "" ''`, `<>` for generics (surrounding pairs only), indentation
  rules keyed on `{`/`}`.
- `src/extension.ts`: already spawns the client (`vscode-languageclient/node`).
  Add:
  - Pass `initializationOptions` (turf.home, feature toggles).
  - Send `workspace/didChangeConfiguration` on settings change.
  - Set `documentSelector` to `{ language: 'turf' }` and watch `**/*.tr` (today
    it watches `**/*.turf` — **wrong extension**; files are `.tr`. Fix.).
  - A "Restart Turf Server" command; status-bar item; output channel for
    `stderr` logs.
  - Bundle or locate the `turf-lsp` binary: default to `turf.server.path`, else
    look on `PATH`, else a bundled platform binary shipped in the VSIX.

### Forks (Cursor, Windsurf, VSCodium)
No code differences — they consume the same VSIX/extension API. Publish to:
- **VS Code Marketplace** (for VS Code, Cursor, Windsurf, which use it), and
- **Open VSX** (for VSCodium and fork users who default to Open VSX).
Test-load the VSIX in at least VS Code + VSCodium (doc 12).

### Semantic tokens + themes
Ensure the advertised legend (doc 04) matches what the extension declares; verify
coloring in a light and a dark theme.

## Neovim client

### Two supported paths
1. **`nvim-lspconfig`** (most users). The existing `editors/neovim/turf-lsp.lua`
   registers `turf_lsp` with `cmd = { 'turf-lsp' }`, `filetypes = { 'turf' }`.
   Improve:
   - `root_dir`: detect project root by a marker (`.git`, or a Turf project file
     if one exists) — the current `find_git_ancestor` is fine as a default.
   - Register the `turf` filetype for `*.tr` (an `ftdetect`/autocmd:
     `vim.filetype.add({ extension = { tr = 'turf' } })`).
   - Pass `init_options` (turf.home) and `settings`.
   - Upstream a `turf_ls` entry to `nvim-lspconfig` so users get it built-in.
2. **Native `vim.lsp.config`/`vim.lsp.enable`** (Neovim 0.11+), no plugin:
   provide a copy-paste snippet using the built-in API for users who avoid
   lspconfig.

### Neovim feature wiring
- Diagnostics: `vim.diagnostic` renders `publishDiagnostics` out of the box
  (virtual text/signs) — verify codes show.
- Semantic tokens: enabled by default in 0.9+; verify our legend colors under a
  Treesitter-less setup (semantic tokens are the highlight source here unless the
  user also has a Treesitter grammar — we ship TextMate for VS Code, Treesitter is
  a separate optional effort, out of scope).
- Completion: works via `vim.lsp.completion` / `nvim-cmp`; ensure snippet
  insert-text (format 2) is honored (document that a snippet engine is needed for
  tab-stops).
- Formatting: `vim.lsp.buf.format()`; document a format-on-save autocmd.
- Position encoding: advertise `utf-8` support so Neovim (which prefers it) avoids
  conversion (doc 01 §6).

## Other editors (documented, smoke-tested only)
- **Emacs** `eglot`/`lsp-mode`: a `.dir-locals`/config snippet mapping `.tr` →
  `turf-mode` (or `prog-mode`) → `turf-lsp`.
- **Sublime** `LSP`: a `LSP-turf` settings snippet.
Provide these as README snippets; they come free from spec compliance.

## Distribution of the server binary
- The clients need a `turf-lsp` executable. Options, document all:
  - User builds it (current: `cmake` in `turf-lsp/`), sets `turf.server.path`.
  - Prebuilt binaries per-platform attached to releases (doc 12); the VS Code
    extension can bundle/download the matching one.
- The binary depends on LLVM 22 (via `turfcore`). Static-link what's feasible so
  the shipped binary doesn't require a system LLVM. Document the build/runtime
  requirements (parallels `turf-lang/AGENTS.md`).

## Phased tasks
- **P0/P5.1** Delete `vscode-turf/`; consolidate into `editors/vscode/`; fix the
  `**/*.turf` → `**/*.tr` watcher; add `language-configuration.json`.
- **P5.2** Extension settings (home, toggles, trace), restart command, status
  bar, output channel, binary discovery/bundling.
- **P5.3** Neovim: ftdetect for `.tr`, lspconfig improvements, native
  `vim.lsp.config` snippet, format-on-save doc.
- **P5.4** Publish to Marketplace + Open VSX; verify in VS Code + VSCodium +
  one fork (Cursor/Windsurf).
- **P5.5** Emacs/Sublime README snippets.

## Acceptance tests
- Fresh VS Code with the VSIX: open a `.tr` file → server starts, diagnostics +
  highlighting + hover + completion + go-to-def all work; format-on-save works.
- Same VSIX loads and works in VSCodium (Open VSX path).
- Neovim 0.10+ with the lspconfig snippet: `.tr` file gets the `turf` filetype,
  server attaches, all features work; `utf-8` encoding negotiated.
- Changing `turf.server.path` / `turf.home` and reloading picks up the new
  values.
