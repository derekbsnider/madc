# TODO

## High Priority

- **Escape sequences in string literals** — `\n`, `\t`, `\r`, `\\`, `\"`, `\0` etc. Currently string literals are stored verbatim by the lexer. Handle backslash escapes in the string tokenizer (`getToken()` in `src/lexer.cpp`). This unblocks `nl2br`, multiline output, and general text processing.

- **`[]` subscript operator for arrays** — `a[0]`, `a["key"]` syntax for MadArray element access. Currently array access is function-based (`php::array_get`). Needs parser support in `parseExpression()` for `tkOpSqr`/`tkClSqr` after an array variable, and compiler support to emit JIT calls to the appropriate MadArray accessor.
