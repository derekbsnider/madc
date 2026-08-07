# Lambda Expressions

Anonymous inline functions. Two spellings are supported: the C++ form and
the madc typed-return form (return type inside the brackets).

## Syntax

```c
int main()
{
	// C++-style void lambda
	auto say = [](string name) { cout << "hi " << name << endl; };
	say("world");

	// madc typed-return lambda (return type inside [])
	auto add = [int](int a, int b) { return a + b; };
	cout << add(17, 25) << endl;

	// capture by reference
	int counter = 0;
	auto inc = [&]() { counter = counter + 1; };
	inc();
	inc();
	cout << counter << endl;
	return 0;
}
```

Output: `hi world`, `42`, `2`.

## Capture

`[&]` captures the enclosing scope by reference — the body reads and
writes the outer variables directly (integers, strings, objects). See
`tests/testcapture.mad` for the covered shapes.

## Return Type

- `[]` — void return
- `[int]`, `[string]`, `[double]` — madc spelling for a typed return

## How It Works

A lambda parses like a function (`parseLambda()`, sharing
`parseFunction()`'s machinery), gets a unique generated name, and is
hoisted to a free function in the `cir_node` tree; the expression's value
is the function's address, usable through `auto` function pointers and
calls. Reference captures pass the captured frame through a hidden
parameter.

## Files

- `src/parser.cpp` — `parseLambda()`, `[` detection in expressions,
  capture handling
- `src/cir_builder.cpp` — hoisted-function lowering
