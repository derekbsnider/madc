# Multiple Return Values

Go-style multiple return values using `return a, b;` and `a, b := func();` syntax.

## Syntax

```c
// function returns multiple values
int divide(int a, int b)
{
    int q = a / b;
    int r = a - (q * b);
    return q, r;
}

// caller unpacks with :=
q, r := divide(17, 5);
```

## Example

```c
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

## How It Works

**Hidden return buffer:** The compiler adds a hidden `__retbuf` parameter to functions that return multiple values. The caller allocates stack space and passes a pointer.

**Return statement:** `return q, r;` writes each value to `[retbuf + i*8]` (int64-sized slots), then returns the first value normally.

**Unpacking:** `a, b := func();` calls the function with the return buffer, then loads each value from the buffer into the declared variables. Types are inferred from the return expressions.

## Conditional Returns

Multiple return values work inside braced if/else blocks:

```c
if (a < b)
{
    return a, b;
}
return b, a;
```

## Known Limitations

- Brace-less `if` with multi-return does not parse correctly -- always use braces
- String return types are not yet supported -- numeric types only (int, double)

## Files

- `src/parser.cpp` -- multi-return parsing, `:=` unpacking
- `src/compiler.cpp` -- `__retbuf` parameter injection, return buffer writes, unpacking loads
