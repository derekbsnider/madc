# Short variable declaration — `x := expr`

`:=` declares a **new** variable whose type is inferred from the initializer
(Go-style). It is a madc-dialect statement, not a C++ feature.

```c
n := 42;            // long n = 42;
s := "hello";       // const char *s = "hello";
q, r := divide(17, 5);   // multi-return receivers — see multiple-returns.md
```

## Scoping rule (owner ruling 2026-09-19)

`:=` follows C++ declaration-statement scoping, which is also Go's rule:

- The variable belongs to the **innermost enclosing block**.
- The substatement of an `if`, `else`, `while`, `do` or `for` is its **own
  block scope** even without braces ([stmt.select]/1, [stmt.iter]/1). A
  `:=` in an unbraced arm dies with the arm:

  ```c
  if ( cond )
          x := 1;
  else
          x := 2;
  println("{}", x);   // error: use of undeclared identifier 'x'
  ```

  Declare before the `if` and assign in the arms instead:

  ```c
  int x;
  if ( cond ) x = 1; else x = 2;
  ```

- A second `:=` of the **same name in the same scope** is an error
  ("'x' is already declared in this scope"). Use `=` to assign an existing
  variable. Every receiver of a multi-return `a, b := f()` is new as well —
  madc does not copy Go's "at least one new variable" exception.
- An inner block may **shadow** an outer variable with `:=`, as in C++.
- In script mode (top-level statements, no `main`), a file-scope `:=` is a
  local of the synthesized `main`, visible to the rest of the script; a
  file-scope `if` arm is still its own scope.

The same rule makes a plain C++ declaration in an unbraced arm
(`if ( c ) int q = 7;`) arm-local, exactly as `g++` and `clang++` scope it.

Tests: `tests/testwalrusscope.mad`, `testwalrusredecl.mad`, `testwalrusarm.mad`,
`testwalrusscriptscope.mad`, `testwalrusscriptlocal.mad`, `testcolon.mad`.
