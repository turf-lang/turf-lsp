# Turf Syntax Reference

A summary for editor work. The language's own reference is `docs/reference/` in
the compiler repository; the reserved words below are what
`turf-lsp --print-keywords` prints, which is the lexer's table.

## Keywords
- **Declarations**: `fn`, `mut`/`mutable`, `immut`/`immutable`, `struct`, `enum`, `machine`, `extern`, `import`, `export`, `static`, `operator`, `pub`/`public`, `unsafe`, `sizeof`, `error`, `caller_location`, `self`
- **Control Flow**: `if`, `elseif`, `else`, `while`, `do`, `for`, `in`, `step`, `compare`, `matches`, `done`, `give`, `otherwise`, `break`, `continue`, `return`
- **Errors and cleanup**: `try`, `recover`, `produce`, `run`, `guess`, `defer`
- **Operators**: `to` (conversion; binds tighter than comparison and `&&`/`||`, looser than arithmetic), `as`

## Types
- `int`, `int32`, `double` (`float` is a synonym), `bool`, `char`, `string`, `ptr`, `void`
- A reference is `&T`; leaving one is `r to T`. A function value is `fn(int): int`, and an array of them is `(fn(int): int)[3]`.

## Literals
- **Booleans**: `true`, `false`
- **Null**: `null`
- **Numbers**: Decimals (`42`, `3.14`, `1_000`, `1e9`), hex `0x`, binary `0b`, octal `0o`
- **Strings and chars**: `"Hello, World!"`, `'a'`; escapes `\n \t \r \0 \a \b \f \v \\ \" \'`

## Modules and the prelude
- `import json;` then `json::parse(...)`. A file imports only what it names: imports are not transitive.
- Nine prelude modules need no import: `int`, `double`, `bool`, `char`, `strings`, `array`, `io`, `math`, `sys`. The last three can also be called by bare name: `printline("hi")`, `abs(-3)`.
- Standard library names are snake_case (`strings::parse_int`, `random::random_int`).

## Comments
- `// This is a comment`, and `/// documentation`

## Special Symbols
- Variadic arguments: `...`
- Annotations: `@caller_location`
