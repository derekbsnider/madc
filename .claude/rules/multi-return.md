# Multiple Return Value Rules

## Parse-Time Detection

- Multi-return is detected in `TokenRETURN::parse()` when `peekToken()` after `parseExpression()` is not `;` and not `ttSymbol`
- `parseExpression` consumes the comma as a stop token — the next expression follows immediately
- When detected, populate `FuncDef::return_types` with `&ddINT64` for each return expression
- Parser param count check must NOT subtract for `__retbuf` — it's injected at compile time, not parse time

## Compile-Time Injection

- `__retbuf` (void* as int64) is injected as the first parameter in `TokenFunc::compile()` when `func->is_multi_return()` returns true
- Check `method.parameters` for existing `__retbuf` before injecting (avoid duplicates)
- Function signature set to `void` return for multi-return functions (`funcsig.setRetT<void>()`)
- Compiler param count check (`expected_argc`) must subtract 1 for `func->is_multi_return()`
- Compiler `param_offset` must be 1 for multi-return (skip hidden `__retbuf` when mapping user args to func params)

## Return Statement

- `TokenRETURN::compile()` must skip `cleanup()` for multi-return paths — avoids double-destruct when multiple return statements exist (if/else branches)
- Write each return value to `[retbuf + i*8]` then `cc.ret()`
- Single-return and void-return paths still call `cleanup()` before returning

## Call Site

- `TokenAssign::compile()` detects multi-return via `multi_vars` vector on the assignment node
- Allocate stack buffer: `cc.newStack(n * 8, 8)`, LEA into a Gp, set as `regdp.object`
- After the call, load each value from `[retbuf + i*8]` into the corresponding variable's operand

## Known Limitations

- Brace-less `if (x) return a, b;` doesn't parse — comma confuses single-statement if body. Always use braces.
- String return types not yet supported — only numeric (int64) slots
