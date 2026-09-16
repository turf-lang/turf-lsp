# Turf Language Server - Emacs Integration

Config snippets only — this comes largely free from LSP spec compliance,
smoke-tested rather than shipped as a package. Both snippets define a
minimal `turf-mode` (no syntax highlighting beyond `prog-mode` defaults;
porting `syntaxes/turf.tmLanguage.json` to Emacs font-lock rules is a
separate, unstarted effort) and attach `turf-lsp` to `.tr` files.

## `eglot` (built into Emacs 29+)

```elisp
(define-derived-mode turf-mode prog-mode "Turf"
  "Major mode for editing Turf source files.")
(add-to-list 'auto-mode-alist '("\\.tr\\'" . turf-mode))

(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               '(turf-mode . ("turf-lsp"))))

(add-hook 'turf-mode-hook #'eglot-ensure)
```

To point at a specific binary or pass `TURF_HOME`, wrap the command:
```elisp
'(turf-mode . ("env" "TURF_HOME=/path/to/turf/home" "/path/to/turf-lsp"))
```

## `lsp-mode`

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
