# TODO

## High Priority

- **Escape sequences in string literals** — `\n`, `\t`, `\r`, `\\`, `\"`, `\0` etc. Currently string literals are stored verbatim by the lexer. Handle backslash escapes in the string tokenizer (`getToken()` in `src/lexer.cpp`). This unblocks `nl2br`, multiline output, and general text processing.

- **`[]` subscript operator for arrays and containers** — `a[0]`, `a["key"]`, `nums[i]`, `ages["bob"]` syntax for element access. Currently array access is function-based (`php::array_get`, `nums.at()`). Needs parser support in `parseExpression()` for `tkOpSqr`/`tkClSqr` after a variable, and compiler support to emit JIT calls to the appropriate accessor.

## Medium Priority

- **`:=` short variable declaration** — Go-style type inference from RHS: `x := 42;` (int), `s := "hello";` (string). Extends the existing `auto` keyword to support general expressions, not just function pointers/lambdas.

- **Lambda capture `[&]`** — Capture enclosing scope variables by reference. Requires passing captured variable addresses as hidden parameters via an environment struct. Currently lambdas are pure (no access to enclosing scope).

- **`madc::` namespace** — Move shared helpers (`array`, future containers) under a `madc::` namespace to avoid polluting the global scope. Keep backward compatibility with `using madc;`.

- **Scope C++ features to `std::` namespace** — `std::vector<int>`, `std::map<string, int>` etc. Currently `vector`/`map`/`set`/`list` are bare keywords. Consider supporting both `vector<int>` and `std::vector<int>`.

- **Register-only iterator** — A special integer iterator type that is inherently a virtual register and never stores to memory. Optimized for tight loops over containers.

## Low Priority

- **Multiple return values** — Go-style `val, err := func()`. High effort, touches calling conventions and the `regdefp_t` system.

- **`switch` statement** — Keyword exists but no compile support.

- **`>>` input operator / `cin`** — Currently `>>` is bitwise right-shift only.

- **Class methods** — Parser detects them but no `this` pointer compilation yet.

- **Regex support** — `grep`/`split` currently use substring match, not regex.
