# Class Methods

Classes can have methods that access member variables through a hidden `this` pointer.

## Syntax

```c
class ClassName {
    type member;

    rettype method(params) {
        // body can access member variables directly
    }
};
```

## Example

```c
class Counter {
    int count;

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
    c.count = 0;
    c.inc();
    c.inc();
    c.inc();
    int n = c.get();
    cout << n << endl;   // output: 3
    return 0;
}
```

## How It Works

**Hidden `__this` parameter:** Each method receives a hidden first argument (`void* __this`) that points to the object instance. Member variable references inside the method body resolve through `__this` plus the member's offset within the struct layout.

**Name mangling:** Method names are mangled as `ClassName__methodName` to avoid collisions with free functions. For the example above, `inc` becomes `Counter__inc` and `get` becomes `Counter__get`.

**Method calls:** `obj.method(args)` compiles to a call to the mangled function with the object's address prepended as the first argument. For stack-allocated objects, LEA is used to compute the address.

## Multiple Instances

Each instance operates on its own data -- the `__this` pointer distinguishes them:

```c
Counter c;
Counter d;
c.count = 0;
d.count = 10;
c.inc();           // c.count -> 1
d.inc();           // d.count -> 11
```

## Files

- `src/parser.cpp` -- class/method parsing, `__this` parameter injection
- `src/compiler.cpp` -- method compilation, member resolution through `__this`
- `include/madc.h` -- class/struct data structures
