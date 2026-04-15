# Switch Statement

C-style switch/case/default with fall-through semantics.

## Syntax

```c
switch (expr) {
    case value:
        // statements
        break;
    default:
        // statements
        break;
}
```

Case values must be literal constants (integers or character literals). The expression is evaluated once, then compared against each case label. Fall-through occurs between cases unless `break` is used.

## Example

```c
int x = 2;

switch (x) {
    case 1:
        cout << "one" << endl;
        break;
    case 2:
        cout << "two" << endl;
        break;
    case 3:
        cout << "three" << endl;
        break;
    default:
        cout << "other" << endl;
        break;
}
// output: two
```

## Default Case

When no case matches, execution jumps to `default:` if present:

```c
x = 99;
switch (x) {
    case 1:
        cout << "one" << endl;
        break;
    default:
        cout << "default" << endl;
        break;
}
// output: default
```

## Fall-Through

Omitting `break` causes execution to continue into the next case:

```c
x = 1;
switch (x) {
    case 1:
        cout << "fall" << endl;
    case 2:
        cout << "through" << endl;
        break;
    case 3:
        cout << "nope" << endl;
        break;
}
// output: fall
//         through
```

## Implementation

- `break` reuses the `loopstack` mechanism -- the switch pushes a tail label onto `pgm.loopstack`, and `break` jumps to it
- Each `case` emits a comparison + conditional jump; `default` is the fallback label

## Files

- `src/parser.cpp` -- `TokenSWITCH::parse()`
- `src/compiler.cpp` -- `TokenSWITCH::compile()`, `TokenCASE::compile()`
