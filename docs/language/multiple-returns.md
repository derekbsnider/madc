# Multiple Return Values

Go-style multiple return values using `return a, b;` and `a, b := func();` syntax.
Values of any copyable type travel — integers, doubles, pointers, and real
class objects (`std::string`) — and a Go-style signature declares
heterogeneous types: `(int, string) lookup();`.

## Syntax

```c
// function returns multiple values
int divide(int a, int b)
{
    int q = a / b;
    int r = a - (q * b);
    return q, r;
}

int main()
{
    // caller unpacks with :=
    q, r := divide(17, 5);
    cout << q << " " << r << endl;   // 3 2
    return 0;
}
```

## Example

```c
int divide(int a, int b)
{
    int q = a / b;
    int r = a - (q * b);
    return q, r;
}

int minmax(int a, int b)
{
    if (a < b)
    {
        return a, b;
    }
    return b, a;
}

int main()
{
    q, r := divide(17, 5);
    cout << q << endl;       // 3
    cout << r << endl;       // 2

    lo, hi := minmax(42, 7);
    cout << lo << endl;      // 7
    cout << hi << endl;      // 42

    return 0;
}
```

## Declared Types

With the bare form (`int divide(...)` + `return q, r;`), **every value
carries the function's declared return type** — exactly C's rule that a
return expression coerces to the declared type, applied per slot. So
`double stats()` returns two doubles and `const char *labels()` returns two
C strings.

The **declared form** gives each slot its own type, class types included:

```c
#include <iostream>
#include <string>
using namespace std;

(int, string) lookup()
{
    string name = "echo";
    return 42, name;
}

int main()
{
    n, who := lookup();
    cout << n << " " << who << endl;   // 42 echo
    return 0;
}
```

Class values are copied with real C++ semantics — the callee's locals are
destroyed, the receivers own their own buffers.

## How It Works

The compiler synthesizes a **transport struct** — `struct { T0 v0; T1 v1; }`
— per slot-type signature; at the C level the function returns that struct.
A trivially-copyable transport (all scalar slots) uses the native struct
return; a transport with class slots takes the same hidden-`__retbuf`
copy-construct/destructor path as any by-value class return. The `:=`
receivers are ordinary locals typed by the slots, filled by plain assignment
(scalars) or the class's real `operator=` (objects).

## Conditional Returns

Multiple return values work in any return position, including brace-less
`if` arms:

```c
int minmax(int a, int b)
{
    if (a < b) return a, b;
    return b, a;
}

int main()
{
    lo, hi := minmax(9, 4);
    cout << lo << " " << hi << endl;   // 4 9
    return 0;
}
```

## Loud Rejects

Misuse is a compile error, never a silent wrong answer:

- wrong receiver count — `'divide' returns 2 values, but 3 receivers given`
- a multi-return call anywhere but an N-receiver `:=` —
  `'divide' returns multiple values; receive them with 'a, b := divide(...)'`
- a `return;` (or single-value return) inside a multi-return function
- `:=` receiving from a non-multi-return callee, a function pointer, or an
  unknown name
- reference or `void` slot types, and multi-return in class methods

## Known Limitations

- The receive form is `a, b := f();` only — a plain `a, b = f();` on
  pre-declared variables is rejected ("Expecting := after identifier
  list").
- Top-level `a, b := f();` in script mode does not yet join the
  synthesized `main` (the receivers surface as undefined imports) —
  unpack inside a function for now.

## Files

- `src/parser.cpp` — multi-return parsing, transport-struct synthesis
  (`multi_return_transport_struct`), `:=` receiver binding + arity gates
- `src/cir_builder.cpp` — return-side slot fills (`translate_return`),
  call-side unpack (`multi_return_unpack`), expression-position gate
- `tests/testmultiret*.mad` — the gates (values + rejects)
