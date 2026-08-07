# Range-Based For Loops

C++-style iteration over madc's `array` and the real C++ containers.

## Syntax

```text
for (type var : container) {
    // body
}
```

## Examples

Over the built-in mixed-type `array`:

```c
array names;
php::array_push(names, "Alice");
php::array_push(names, "Bob");

for (string name : names)
	cout << name << endl;

array nums;
php::array_push_int(nums, 10);
php::array_push_int(nums, 20);
int sum = 0;
for (int n : nums)
	sum = sum + n;
cout << sum << endl;
```

Output: `Alice`, `Bob`, `30`.

Over real C++ containers:

```c
#include <vector>

int main()
{
	vector<int> v;
	v.push_back(40);
	v.push_back(2);
	int total = 0;
	for (int n : v)
		total = total + n;
	cout << total << endl;
	return 0;
}
```

Output: `42`.

## How It Works

The parser detects `for (type var :` and produces a `TokenFOREACH` node.
Lowering depends on the range:

- **`array`** — an index-based loop over the MadArray helpers
  (`count` / element getters)
- **C++ containers** — the standard begin/end iterator protocol against
  the real container methods

`break` and `continue` work in both forms (the loop shares the
loop-stack labels with the classic `for`).

## Files

- `include/tokens.h` — `TokenFOREACH`
- `src/parser.cpp` — detection in `TokenFOR::parse()`
- `src/cir_builder.cpp` — both lowerings
