# Switch Statement

C-style `switch` / `case` / `default` with fall-through semantics.

## Syntax

```text
switch (expr) {
    case value:
        // statements
        break;
    default:
        // statements
        break;
}
```

Case labels take integral constant expressions — literals, character
constants, enumerators (including namespace-qualified scoped-enum
constants), and folded `constexpr` values. The expression is evaluated
once; fall-through occurs between cases unless `break` intervenes.

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
```

Output: `two`

## Default Case

When no case matches, execution jumps to `default:` if present:

```c
int x = 99;
switch (x) {
	case 1:
		cout << "one" << endl;
		break;
	default:
		cout << "default" << endl;
		break;
}
```

Output: `default`

## Fall-Through

Omitting `break` continues into the next case:

```c
int x = 1;
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
```

Output: `fall` then `through`.

## Implementation

- `switch` lowers to the C11 switch in the `cir_node` tree; c2mir owns the
  branch codegen (including its range-case extension used elsewhere).
- `break` shares the loop-stack mechanism at parse time — the switch
  pushes its tail label, `break` binds to it.

## Files

- `src/parser.cpp` — `TokenSWITCH::parse()`, case-label constant folding
- `src/cir_builder.cpp` — switch/case lowering to the C11 tree
