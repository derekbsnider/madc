# Defer Statement

Go-style deferred execution — register a statement to run at scope exit in LIFO order.

## Syntax

```c
defer statement;
```

## How It Works

`TokenDEFER::parse()` reads the next statement and stores it on the enclosing `TokenCpnd`'s `deferred` vector. At scope exit, `cleanup()` compiles the deferred statements in reverse (LIFO) order **before** running destructors — so deferred code can still access scope variables.

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

## Implementation

- `TokenCpnd::deferred` — `vector<TokenBase*>` stores deferred statements
- `cleanup(Program&)` — compiles deferred in reverse order before destructor loop
- `parseFunction()` / `parseLambda()` — copy `tc->deferred` to `tf->deferred`

## Files

- `include/tokens.h` — `TokenDEFER` class
- `include/madc.h` — `deferred` vector on `TokenCpnd`
- `src/parser.cpp` — `TokenDEFER::parse()`
- `src/compiler.cpp` — `cleanup()` deferred compilation
