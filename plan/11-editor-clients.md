# 11 — Editor clients (VS Code + forks, Neovim)

## Goal
Ship first-class clients for **VS Code and its forks** (Cursor, Windsurf,
VSCodium) and **Neovim**, plus documented config for other LSP editors. The
server is editor-agnostic; these are thin.

## Current state (consolidate first)
✅ Done (2026-09-16): there used to be two overlapping VS Code extension
trees (`editors/vscode/` and a near-duplicate `vscode-turf/`).
`vscode-turf/` is deleted (kept in git history); `editors/vscode/` is now
the single canonical extension, carrying forward everything that was
unique to `vscode-turf/` (its richer `turf.tmLanguage.json`, its
`language-configuration.json`, its workspace-relative binary discovery)
minus one bug: `vscode-turf`'s grammar/language-configuration both
claimed `/* */` block comments exist, but the lexer only implements `//`
line comments (confirmed in `Lexer.cpp`) — dropped that pattern rather
than carrying forward a highlighting bug. `turf-lang/grammars/` was
confirmed unrelated (`slm_suggestion.gbnf`, a different grammar format
for a different purpose) and left alone.

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
✅ Done (2026-09-16), both paths:
1. **`nvim-lspconfig`**: `editors/neovim/turf-lsp.lua` now exposes a
   `require('turf-lsp').setup(opts)` wrapping the `turf_lsp` registration,
   with `opts.cmd` (binary override), `opts.turf_home` (threaded through as
   `cmd_env = { TURF_HOME = ... }` — the compiler env var, not an LSP
   `initializationOption`; the server doesn't read those today, same
   reasoning as doc 11's VS Code `turf.home` setting), `on_attach`,
   `capabilities`. `root_dir` keeps `find_git_ancestor`, per the original
   plan note that it's fine as a default.
2. **Native `vim.lsp.config`/`vim.lsp.enable`** (Neovim 0.11+):
   `editors/neovim/native.lua`, a drop-in `lsp/turf_lsp.lua` config table.
3. **`editors/neovim/ftdetect.lua`** (new, shared by both paths): registers
   the `turf` filetype for `.tr` — previously missing entirely, so neither
   path could ever actually attach to a `.tr` buffer (`filetypes = {
   'turf' }` had nothing to match against). The README's own setup
   instructions also said "Open a `.turf` file" — same bug as the VS Code
   extension's watcher, fixed here too.
- Position encoding: not changed — confirmed by reading the server's
  `negotiatePositionEncoding`/`positionEncodings` handling (doc 01 §6, `src/LSPHandler.cpp`)
  that this is already spec-negotiated automatically; documented in the
  README rather than adding a no-op client-side override.
- Upstreaming a `turf_ls` entry to `nvim-lspconfig` itself: **not done** —
  an external PR against a repo this session doesn't own, documented as a
  follow-up in the README rather than attempted.

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
- ✅ **P0/P5.1** Done (2026-09-16): deleted `vscode-turf/`; consolidated into
  `editors/vscode/`; fixed the `**/*.turf` → `**/*.tr` watcher; added
  `language-configuration.json`.
- ✅ **P5.2** Done (2026-09-16): extension settings (`turf.home`,
  `turf.trace.server`, `turf.diagnostics.enable`, `turf.format.onSave`,
  `turf.completion.snippets`), restart command, status bar item, binary
  discovery (workspace-relative candidates → dev-mode fallback → PATH).
  Bundling a prebuilt binary in the VSIX is **not done** — no release
  pipeline producing per-platform binaries exists yet (doc 12); output
  channel needed no extra code (`vscode-languageclient` already pipes
  child-process stderr into the client's default output channel).
- ✅ **P5.3** Done (2026-09-16): Neovim `ftdetect.lua` for `.tr`,
  `turf-lsp.lua` lspconfig wrapper (`cmd`/`turf_home`/`on_attach`/
  `capabilities` opts), `native.lua` for the 0.11+ `vim.lsp.config` path,
  format-on-save documented as a README snippet (not auto-installed —
  editor-agnostic clients shouldn't silently add autocmds to someone's
  config). Upstreaming to `nvim-lspconfig` itself not done (external PR,
  documented as a follow-up).
- ⬜ **P5.4** Not done, deliberately: publish to Marketplace + Open VSX;
  verify in VS Code + VSCodium + one fork. Needs external publisher
  credentials/accounts — an action requiring explicit user authorization,
  not something to do autonomously.
- ✅ **P5.5** Done (2026-09-16): `editors/emacs/README.md` (eglot +
  lsp-mode snippets) and `editors/sublime/README.md` (LSP package
  settings snippet). Both are honest about what's *not* included: neither
  ships syntax highlighting (no font-lock rules for Emacs, no
  `.sublime-syntax` for Sublime — porting `turf.tmLanguage.json` to either
  is unstarted); LSP features (diagnostics/hover/completion/go-to-def)
  work via the snippets as given.

## Acceptance tests
- ⬜ Fresh VS Code with the VSIX: open a `.tr` file → server starts,
  diagnostics + highlighting + hover + completion + go-to-def all work;
  format-on-save works. **Not run** — no GUI VS Code available in this
  environment; verified instead via `tsc -p ./` compiling clean and
  manual code review of the capability/handler wiring against the
  already-live-tested server (docs 03-10).
- ⬜ Same VSIX loads and works in VSCodium (Open VSX path). Not run, same
  reason; no VSIX has been packaged at all yet (doc 12 P5.4 territory).
- ⬜ Neovim 0.10+ with the lspconfig snippet: `.tr` file gets the `turf`
  filetype, server attaches, all features work; `utf-8` encoding
  negotiated. **Not run** — no Neovim available in this environment;
  verified instead via `luac -p` syntax-checking all three `.lua` files
  and reading the server's own position-encoding negotiation code (doc
  01 §6, confirmed already spec-compliant, nothing Neovim-specific to add).
- ⬜ Changing `turf.server.path` / `turf.home` and reloading picks up the
  new values. VS Code side wires a `workspace/onDidChangeConfiguration`
  restart for exactly these two settings; not run live (same GUI
  limitation as above).
