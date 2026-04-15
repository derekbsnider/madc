# Ternary Operator

Conditional expressions using `condition ? true_expr : false_expr`.

## Syntax

```c
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

## Precedence

The ternary operator has precedence level 13 -- lower than comparison operators, higher than assignment. This means `x > 3 ? 100 : 200` parses as `(x > 3) ? 100 : 200` without needing parentheses.

## Implementation

The compiler uses a stack-slot merge strategy: both branches write their result to the same stack memory location, and the final value is loaded from that slot after the conditional. This avoids asmjit register convergence issues where two code paths would need to produce a value in the same virtual register.

## Files

- `src/parser.cpp` -- ternary detection and parsing in expression handler
- `src/compiler.cpp` -- conditional jump emission, stack-slot merge
