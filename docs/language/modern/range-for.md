# Range-Based For Loops

C++ style iteration over MadArray containers.

## Syntax

```c
for (type var : container) {
    // body
}
```

## How It Works

The parser detects `for (type var :` in `TokenFOR::parse()` and produces a `TokenFOREACH` AST node instead of `TokenFOR`. The compiler emits an index-based loop calling `php_count()` and `php_array_get()` / `php_array_get_int()` from the existing MadArray helpers.

The loop variable is added to the enclosing scope during parsing so it's visible inside the body. The loop index register (`foreach_idx`) is a virtual register that never touches memory — it's inherently register-only.

## Supported Element Types

- `string` — calls `php_array_get(result, arr, index)` per iteration
- `int` — calls `php_array_get_int(arr, index)` per iteration

## Break / Continue

Both work — the loop pushes `(forcont, fortail)` labels onto `pgm.loopstack`, same as traditional for.

## Files

- `include/tokens.h` — `TokenFOREACH` class
- `src/parser.cpp` — detection in `TokenFOR::parse()`
- `src/compiler.cpp` — `TokenFOREACH::compile()`
