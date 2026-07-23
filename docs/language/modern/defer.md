# Defer Statement

Go-style deferred execution — register a statement to run at scope exit in LIFO order.

## Syntax

```c
defer statement;
```

## How It Works

`TokenDEFER::parse()` reads the next statement and stores it on the enclosing `TokenCpnd`'s `deferred` vector. The CIR builder emits the deferred statements inline at each exit of that compound — the fall-off end, and before every `return` inside it (innermost scope first, each scope's list in reverse registration = LIFO order) — **before** destructors run (destructors ride the c2mir `cleanup` attribute, which fires at the actual scope exit), so deferred code can still access scope variables.

A `return <expr>` evaluates the expression **before** the deferred statements run (Go ordering): the value is hoisted into a temporary of the function's return type, the deferred statements execute, then the temporary is returned.

## Example

```c
ofstream out;
string fname = "output.txt";
out.open(fname);
defer out.close();         // runs at scope exit, after all other code
out << "data" << endl;     // runs before the deferred close
```

## LIFO Order

```c
defer cout << "third" << endl;   // runs last
defer cout << "second" << endl;  // runs second
cout << "first" << endl;          // runs first
// output: first, second, third
```

## Limitations

- `break` / `continue` / `goto` that leave a defer-carrying scope do not run
  that scope's deferred statements (only fall-off and `return` do). The old
  asmjit backend had the same behavior.
- Deferred statements do not run on an exception (longjmp) unwind path.

## Implementation

- `TokenCpnd::deferred` — `vector<TokenBase*>` stores deferred statements
- `CirBuilder::append_deferred_stmts()` — emits pending deferred statements
  (LIFO) at a compound's fall-off end (`translate_block`) and before each
  `return` (`translate_return`, all active scopes)
- `parseFunction()` / `parseLambda()` — copy `tc->deferred` to `tf->deferred`

## Files

- `include/tokens.h` — `TokenDEFER` class
- `include/madc.h` — `deferred` vector on `TokenCpnd`
- `src/parser.cpp` — `TokenDEFER::parse()`
- `src/cir_builder.cpp` — `append_deferred_stmts()`, `translate_block`,
  `translate_return` defer emission
