-- Turf LSP Neovim Configuration
-- 
-- 1. Ensure you have the `turf-lsp` executable in your PATH.
-- 2. If you use `nvim-lspconfig`, you can add this custom server configuration:

local lspconfig = require 'lspconfig'
local configs = require 'lspconfig.configs'

-- Check if the config is already defined
if not configs.turf_lsp then
  configs.turf_lsp = {
    default_config = {
      cmd = { 'turf-lsp' },
      filetypes = { 'turf' },
      root_dir = function(fname)
        return lspconfig.util.find_git_ancestor(fname) or vim.loop.os_homedir()
      end,
      settings = {},
    },
  }
end

-- Setup the server
lspconfig.turf_lsp.setup {
  -- Add your custom on_attach or capabilities here
  -- on_attach = my_custom_on_attach,
  -- capabilities = my_custom_capabilities,
}
