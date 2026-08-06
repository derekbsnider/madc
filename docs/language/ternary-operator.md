# Ternary Operator

Conditional expressions using `condition ? true_expr : false_expr`.

## Syntax

```text
type var = condition ? true_expr : false_expr;
```

## Example

```c
int x = 5;

// basic ternary in assignment
int y = x > 3 ? 100 : 200;
cout << y << endl;         // 100

// false branch
int z = x < 3 ? 100 : 200;
cout << z << endl;         // 200

// expressions in branches
int a = 10;
int b = a > 5 ? a + 1 : a - 1;
cout << b << endl;         // 11

// literal condition
int c = 0 ? 999 : 42;
cout << c << endl;         // 42
```

## Where It Works

- Variable initialization: `int y = x > 3 ? 100 : 200;`
- Return statements: `return x > 0 ? 1 : 0;`
- Expressions with arithmetic in branches: `a > 5 ? a + 1 : a - 1`
- C++ lvalue conditionals: when both arms are lvalues of one type, the
  result is an lvalue (`(flag ? a : b) = v;` assigns through), matching
  g++/clang++

## Precedence

Standard C placement — lower than comparison operators, higher than
assignment: `x > 3 ? 100 : 200` parses as `(x > 3) ? 100 : 200` without
parentheses.

## Implementation

The ternary lowers to a C11 conditional expression in the `cir_node`
tree; c2mir/MIR own the branch and merge codegen. The C++
lvalue-conditional case distributes the address-of into the arms during
lowering.

## Files

- `src/parser.cpp` — ternary parsing in the expression handler
- `src/cir_builder.cpp` — conditional lowering (including the lvalue
  form)
