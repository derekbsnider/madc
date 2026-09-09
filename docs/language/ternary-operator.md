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
- The value carrier (`var`) against a scalar or a char pointer:
  `c ? v : php::trim(p)`, `c ? "lit" : v`, `x.is_null() ? 1 : x` — the
  conditional is a value ([expr.cond]/4: the class arm wins the implicit
  conversion, as `c ? s : trim(p)` is a `std::string` in C++). Only the
  selected arm is evaluated. Two `var` lvalues stay an lvalue conditional.

## Precedence

Standard C placement — lower than comparison operators, higher than
assignment: `x > 3 ? 100 : 200` parses as `(x > 3) ? 100 : 200` without
parentheses.

## Implementation

The ternary lowers to a C11 conditional expression in the `cir_node`
tree; c2mir/MIR own the branch and merge codegen. The C++
lvalue-conditional case distributes the address-of into the arms during
lowering. A value prvalue (one carrier arm, one converting arm) is
materialized as a scope-local carrier temporary the conditional itself
assigns — `(c ? assign(tmp, A) : assign(tmp, B), tmp)` through the
carrier's registered `operator=` rows — so the unselected arm never runs.

## Files

- `src/parser.cpp` — ternary parsing in the expression handler
- `src/cir_builder.cpp` — conditional lowering (including the lvalue
  form)
