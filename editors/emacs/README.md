# Turf in Emacs

Configuration snippets for `eglot` and `lsp-mode`; nothing to install. Both define a
minimal `turf-mode` derived from `prog-mode` and attach `turf-lsp` to `.tr` files.
`turf-mode` has no syntax highlighting of its own; see Highlighting below.

Get the server first: take `turf-lsp-<platform>-<version>` from a
[release](https://github.com/turf-lang/turf-lsp/releases), and put its `bin/` on your
`exec-path`, or use the full path in the snippets. Keep the archive's layout: the
server finds the standard library at `../lib/std` from its own directory, so
`TURF_HOME` is not needed. See the [main README](../../README.md#other-editors).

## eglot (built into Emacs 29 and later)

```elisp
(define-derived-mode turf-mode prog-mode "Turf"
  "Major mode for editing Turf source files.")
(add-to-list 'auto-mode-alist '("\\.tr\\'" . turf-mode))

(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               '(turf-mode . ("turf-lsp"))))
(add-hook 'turf-mode-hook #'eglot-ensure)
```

To name the binary, or to take the standard library from a different installation,
wrap the command:

```elisp
'(turf-mode . ("env" "TURF_HOME=/path/to/turf/home" "/path/to/turf-lsp"))
```

## lsp-mode

```elisp
(define-derived-mode turf-mode prog-mode "Turf"
  "Major mode for editing Turf source files.")
(add-to-list 'auto-mode-alist '("\\.tr\\'" . turf-mode))

(with-eval-after-load 'lsp-mode
  (add-to-list 'lsp-language-id-configuration '(turf-mode . "turf"))
  (lsp-register-client
   (make-lsp-client :new-connection (lsp-stdio-connection "turf-lsp")
                    :major-modes '(turf-mode)
                    :server-id 'turf-lsp)))
(add-hook 'turf-mode-hook #'lsp-deferred)
```

## Highlighting

The server provides semantic tokens. `lsp-mode` uses them when
`lsp-semantic-tokens-enable` is on; `eglot` does not use them, so `turf-mode` is
unhighlighted there. There is no font-lock definition for Turf. The VS Code
extension's TextMate grammar (`editors/vscode/syntaxes/turf.tmLanguage.json`) is
the reference for the language's tokens if you want to write one.
