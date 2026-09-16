-- Turf LSP Neovim configuration (nvim-lspconfig path)
--
-- 1. Ensure the `turf-lsp` executable is built and on your PATH (or pass an
--    explicit path via M.setup({ cmd = { '/path/to/turf-lsp' } })).
-- 2. `require('ftdetect')` (or inline its contents) BEFORE calling
--    M.setup(), so `.tr` buffers are recognized as `turf` filetype.
-- 3. require('turf-lsp').setup() in your init.lua.

local M = {}

-- opts:
--   cmd          - override the command table (default { 'turf-lsp' })
--   turf_home    - sets TURF_HOME in the spawned server's environment, used
--                  for stdlib/installed-package import resolution (this is
--                  a compiler env var, not an LSP initializationOption --
--                  the server doesn't read initializationOptions today, so
--                  env is the only mechanism that actually does anything)
--   on_attach    - passed through to lspconfig
--   capabilities - passed through to lspconfig (merge with your completion
--                  plugin's capabilities, e.g. cmp_nvim_lsp, if you use one)
function M.setup(opts)
  opts = opts or {}

  local lspconfig = require 'lspconfig'
  local configs = require 'lspconfig.configs'

  if not configs.turf_lsp then
    configs.turf_lsp = {
      default_config = {
        cmd = opts.cmd or { 'turf-lsp' },
        filetypes = { 'turf' },
        root_dir = function(fname)
          return lspconfig.util.find_git_ancestor(fname) or vim.loop.os_homedir()
        end,
        settings = {},
      },
    }
  end

  local setup_opts = {
    on_attach = opts.on_attach,
    capabilities = opts.capabilities,
  }
  if opts.turf_home then
    setup_opts.cmd_env = { TURF_HOME = opts.turf_home }
  end

  lspconfig.turf_lsp.setup(setup_opts)
end

return M
