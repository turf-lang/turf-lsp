# Turf Language Server - Neovim Integration

Config snippets for attaching `turf-lsp` to Neovim. No plugin to install —
copy what you need into your own config.

## 0. Filetype detection (required either way)

Neovim has no built-in `turf` filetype. Without registering one, `.tr`
buffers never get the `turf` filetype, `filetypes = { 'turf' }` never
matches, and the server never attaches — silently. Add this early in your
init.lua (or `require('ftdetect')` if you copy `ftdetect.lua` into your
config directory):

```lua
vim.filetype.add({ extension = { tr = 'turf' } })
```

## 1. `nvim-lspconfig` (most users)

1. Get `turf-lsp` and put it on your `$PATH`. Keep it in a `bin/` directory with
   the standard library at `../lib/std` (the layout of a release, and of the VS
   Code extension's `server/` directory): the server finds the library next to
   itself, so no `TURF_HOME` is needed. See the [main README](../../README.md).
2. Copy `turf-lsp.lua` into your config (e.g. as a Lua module), then:
   ```lua
   require('turf-lsp').setup()
   -- or, with options:
   require('turf-lsp').setup({
     cmd = { '/absolute/path/to/turf-lsp' },
     turf_home = '/path/to/turf/home', -- optional, sets TURF_HOME for the server
     on_attach = my_on_attach,
     capabilities = my_capabilities, -- e.g. from cmp_nvim_lsp
   })
   ```

## 2. Native `vim.lsp.config`/`vim.lsp.enable` (Neovim 0.11+, no plugin)

1. Save `native.lua` as `~/.config/nvim/lsp/turf_lsp.lua`.
2. In your init.lua: `vim.lsp.enable('turf_lsp')`.

Edit the `cmd`/`cmd_env` fields directly in that file for a custom binary
path or `TURF_HOME`.

## Feature notes

- **Diagnostics**: rendered automatically via `vim.diagnostic` (virtual
  text/signs) once the server attaches.
- **Semantic tokens**: on by default in Neovim 0.9+; this is the highlight
  source in Neovim (we ship a TextMate grammar for VS Code, not a
  Treesitter grammar for Neovim — out of scope for now).
- **Completion**: works via `vim.lsp.completion` or `nvim-cmp`. Our
  snippet-style completions (call argument placeholders) need a snippet
  engine to expand tab stops (e.g. `nvim-cmp` + `LuaSnip`/`vsnip`); without
  one, they still insert but the tab stops are inert text.
- **Formatting**: `vim.lsp.buf.format()`. For format-on-save:
  ```lua
  vim.api.nvim_create_autocmd('BufWritePre', {
    pattern = '*.tr',
    callback = function(args)
      vim.lsp.buf.format({ bufnr = args.buf, async = false })
    end,
  })
  ```
- **Position encoding**: negotiated automatically per the LSP spec —
  turf-lsp advertises `utf-8` support and Neovim (0.10+) prefers it, so no
  client-side `offset_encoding` override is needed.

## Upstreaming

Not done in this pass: submitting a `turf_ls` entry to `nvim-lspconfig`
upstream so it's registered out of the box. That's a PR against an
external repo, left for a maintainer to send when ready, not something to
do unprompted from here.
