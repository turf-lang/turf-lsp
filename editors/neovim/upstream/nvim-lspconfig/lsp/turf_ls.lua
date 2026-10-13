---@brief
---
--- https://github.com/turf-lang/turf-lsp
---
--- The language server for the Turf programming language. Install it with
--- `:MasonInstall turf-lsp`, or take `turf-lsp-<platform>-<version>` from a
--- [release](https://github.com/turf-lang/turf-lsp/releases) and keep its
--- layout (`bin/turf-lsp` next to `lib/std`): the server finds the standard
--- library next to itself.
---
--- Neovim does not detect Turf files (it reads `.tr` as `nroff`), so register
--- the filetype first:
---
--- ```lua
--- vim.filetype.add({ extension = { tr = 'turf' } })
--- ```

---@type vim.lsp.Config
return {
  cmd = { 'turf-lsp' },
  filetypes = { 'turf' },
  root_markers = { 'turf.toml', '.git' },
}
