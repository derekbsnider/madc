# Lambda Expressions

Anonymous inline functions that produce function pointer values.

## Syntax

```c
// void lambda
auto fn = [](string name) { cout << name << endl; };

// typed-return lambda (return type inside [])
auto add = [int](int a, int b) { return a + b; };
```

## How It Works

**Parsing:** `parseLambda()` is triggered when `[` appears in an expression context. It parses the optional return type, parameter list, and body — following the same pattern as `parseFunction()`. The lambda gets a unique auto-generated name (`__lambda_0`, `__lambda_1`, ...).

**AST hoisting:** The lambda is pushed onto `pgm.ast` as a top-level `TokenFunc` during parsing of the enclosing function. Since `ast` is FIFO and the enclosing function's `ast.push` happens after `parseCompound` returns, the lambda compiles first — its `FuncNode` label is available when the enclosing function references it.

This hoisting is required because asmjit cannot nest `addFunc()`/`endFunc()` calls.

**Compilation:** The lambda compiles identically to a named function via `TokenFunc::compile()`. The caller gets a `TokenVar` referencing the lambda's function variable, which emits the function's address through the Phase 2 function-pointer machinery.

## Return Type

- `[]` — void return (default)
- `[int]` — returns int
- `[string]` — returns string

## Limitations (V1)

- No capture semantics (`[&]` not yet supported)
- Lambdas cannot access variables from the enclosing scope
- Return type must be specified explicitly (no inference from `return` statements)

## Files

- `include/madc.h` — `parseLambda()` declaration
- `src/parser.cpp` — `parseLambda()` implementation, `[` detection in `parseExpression()`, lambda support in `auto` handler
