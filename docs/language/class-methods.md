# Class Methods

Classes have methods that access member variables through a hidden `this`
pointer. Members of a `class` default **private** (a `struct`'s default
public), exactly as in C++ — access control is enforced.

## Syntax

```text
class ClassName {
    type member;                  // private by default

public:
    rettype method(params) {
        // the body reads/writes members directly
    }
};
```

## Example

```c
class Counter {
	int count;

public:
	Counter() : count(0) {}

	void inc()
	{
		count = count + 1;
	}

	int get()
	{
		return count;
	}
};

int main()
{
	Counter c;
	c.inc();
	c.inc();
	c.inc();
	cout << c.get() << endl;
	return 0;
}
```

Output: `3`

## Multiple Instances

Each instance operates on its own data — the `this` pointer distinguishes
them:

```c
class Tally {
	int n;
public:
	Tally(int start) : n(start) {}
	void inc() { n = n + 1; }
	int get() { return n; }
};

int main()
{
	Tally a(0);
	Tally b(10);
	a.inc();
	b.inc();
	cout << a.get() << " " << b.get() << endl;
	return 0;
}
```

Output: `1 11`

## How It Works

- **Hidden `__this` parameter:** each method receives a hidden first
  argument pointing at the object; member references in the body resolve
  as offsets from it.
- **Symbol naming:** madc-defined methods emit as `ClassName__methodName`;
  methods of classes that come from real C++ headers (`std::string`, the
  containers) bind mangled-direct to the real library's Itanium symbols
  instead.
- **Calls:** `obj.method(args)` passes the object's address as the hidden
  first argument. Virtual methods dispatch through the Itanium vtable —
  see [supported C++ features](cpp-features.md) for inheritance, virtual
  dispatch, and construction/destruction.

## Files

- `src/parser.cpp` — class/method parsing, access control, `__this`
  parameter injection
- `src/cir_builder.cpp` — method lowering, member resolution through
  `__this`, vtables
- `include/madc.h`, `include/datadef.h` — class/struct data structures
