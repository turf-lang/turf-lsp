-- Turf LSP Neovim configuration, no plugin (Neovim 0.11+ native
-- vim.lsp.config / vim.lsp.enable API), for users who don't use
-- nvim-lspconfig.
--
-- Setup:
-- 1. `require('ftdetect')` (or inline its contents) somewhere in your
--    init.lua so `.tr` buffers are recognized as `turf` filetype.
-- 2. Save this file as ~/.config/nvim/lsp/turf_lsp.lua (the name of the
--    file under lsp/ becomes the server's name for vim.lsp.enable).
-- 3. In init.lua: vim.lsp.enable('turf_lsp')

---@type vim.lsp.Config
return {
  cmd = { 'turf-lsp' },
  filetypes = { 'turf' },
  -- turf.toml first: a project's imports resolve from its root.
  root_markers = { 'turf.toml', '.git' },
  -- Neovim 0.10+ negotiates positionEncoding with the server per the LSP
  -- spec (general.positionEncodings in the client's initialize request);
  -- turf-lsp advertises utf-8 support and Neovim prefers it, so no
  -- offset_encoding override is needed here.

  -- To pass TURF_HOME (stdlib/installed-package import resolution) to the
  -- server process, uncomment:
  -- cmd_env = { TURF_HOME = '/path/to/turf/home' },
}
