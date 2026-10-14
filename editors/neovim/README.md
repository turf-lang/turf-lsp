# Turf in Neovim

Configuration for attaching `turf-lsp` to Neovim. There is no plugin to install:
copy what you need into your own config. The files in this directory are the
snippets; [`upstream/`](upstream/) holds what is submitted to Mason and
nvim-lspconfig.

## 1. Get the server

Take `turf-lsp-<platform>-<version>` from a
[release](https://github.com/turf-lang/turf-lsp/releases), unpack it, and put its
`bin/` on your `PATH`, or give the full path in `cmd` below. Keep the archive's
layout (`bin/turf-lsp` with `lib/std` beside `bin/`): the server finds the standard
library next to itself, so `TURF_HOME` is not needed. See the
[main README](../../README.md#other-editors).

## 2. Register the filetype

Neovim reads `.tr` as `nroff` and has no `turf` filetype. Without one, the server
never attaches, and nothing says why. Put this early in your `init.lua`, or copy
[`ftdetect.lua`](ftdetect.lua) into your config and `require('ftdetect')`:

```lua
vim.filetype.add({ extension = { tr = 'turf' } })
```

## 3. Attach the server

### Neovim 0.11 or later, no plugin

1. Save [`native.lua`](native.lua) as `~/.config/nvim/lsp/turf_lsp.lua`.
2. In `init.lua`: `vim.lsp.enable('turf_lsp')`.

Edit `cmd` in that file for a specific binary, and uncomment `cmd_env` to set
`TURF_HOME`.

### With nvim-lspconfig

Copy [`turf-lsp.lua`](turf-lsp.lua) into your config as a Lua module, then:

```lua
require('turf-lsp').setup()
-- or, with options:
require('turf-lsp').setup({
  cmd = { '/absolute/path/to/turf-lsp' },
  turf_home = '/path/to/turf/home', -- optional: sets TURF_HOME for the server
  on_attach = my_on_attach,
  capabilities = my_capabilities,   -- for example from cmp_nvim_lsp
})
```

Both attach to `turf` buffers and use the directory with a `turf.toml` (or `.git`)
as the root, because a project's imports resolve from its manifest.

## Notes

- **Diagnostics** appear through `vim.diagnostic` once the server attaches.
- **Highlighting** comes from the server's semantic tokens, which Neovim uses by
  default. There is no Tree-sitter grammar for Turf.
- **Completion** works through `vim.lsp.completion` or nvim-cmp. Completions with
  call-argument tab stops need a snippet engine to expand them (nvim-cmp with
  LuaSnip or vsnip); without one they insert, and the tab stops stay as plain text.
  Passing `init_options = { completionSnippets = false }` to the server turns the
  snippet completions off.
- **Formatting** is `vim.lsp.buf.format()`. To format on save:

  ```lua
  vim.api.nvim_create_autocmd('BufWritePre', {
    pattern = '*.tr',
    callback = function(args)
      vim.lsp.buf.format({ bufnr = args.buf, async = false })
    end,
  })
  ```
- **Position encoding** is negotiated. The server supports `utf-8` and `utf-16`,
  and Neovim 0.10 and later prefers `utf-8`, so no `offset_encoding` override is
  needed.

## Upstreaming

Two files are meant to be submitted to other projects so that users need none of the
above except the filetype line:

- [`upstream/mason-registry/packages/turf-lsp/package.yaml`](upstream/mason-registry/packages/turf-lsp/package.yaml)
  goes to [mason-org/mason-registry](https://github.com/mason-org/mason-registry).
  It installs the release archives `turf-lsp-<platform>-<version>` from the tag it
  names, currently `v1.0.0`, for `linux_x64_gnu`, `darwin_arm64` and `win_x64`. Once
  it is merged, `:MasonInstall turf-lsp` works. For a new release, change the tag
  in `source.id`.
- [`upstream/nvim-lspconfig/lsp/turf_ls.lua`](upstream/nvim-lspconfig/lsp/turf_ls.lua)
  goes to [neovim/nvim-lspconfig](https://github.com/neovim/nvim-lspconfig) as
  `lsp/turf_ls.lua`. Once it is merged, `vim.lsp.enable('turf_ls')` needs no copied
  config, and mason-lspconfig maps `turf-lsp` to it (the `neovim.lspconfig` key in
  `package.yaml`).

Neither is merged yet. Both need the release to exist, which `release.yml`
publishes when a `vX.Y.Z` tag is pushed. The filetype line stays the user's job
either way.
