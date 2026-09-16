-- Registers the `turf` filetype for `.tr` files. Neither `nvim-lspconfig`'s
-- `filetypes = { 'turf' }` nor the native `vim.lsp.config` path (see
-- native.lua) will ever attach to a `.tr` buffer without this -- Neovim has
-- no built-in notion of a "turf" filetype, so it's silently never set,
-- and the server never starts, unless something registers it first.
--
-- Add this to your init.lua (or `require` this file) BEFORE the lspconfig
-- setup in turf-lsp.lua / your native.lua config, so the filetype exists by
-- the time a `.tr` buffer is opened.
vim.filetype.add({
  extension = {
    tr = 'turf',
  },
})
