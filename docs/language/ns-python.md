# python:: Namespace

Python-unique string operations: title case, case swapping, string alignment, zero-padding, character class testing, substring counting, and `format()` with `{}` placeholders.

## Dialect (lean) forms — the primary surface

Available in every madc TU with zero includes (dialect-lean,
2026-08-21), with Python's real semantics: strings are immutable, every
method returns a NEW string. Text returns are ring-lifetime
`const char *`; subjects can be a `value` or a `const char *`:
`python::title(s)`, `swapcase(s)`, `center(s, w, fill)`,
`ljust(s, w, fill)`, `rjust(s, w, fill)`, `zfill(s, w)`,
`replace(s, old, new)` — plus the value-out
`python::format(out, fmt, args)`. The `std::string`-flavored forms are
C++-interop conveniences, declared only when `<string>` precedes
`<ns_python>`.

## Case Transforms

| Function | Description | Example |
|----------|-------------|---------|
| `title(str)` | Title Case every word | `python::title(s)` — "hello world" -> "Hello World" |
| `swapcase(str)` | Swap upper/lower case | `python::swapcase(s)` — "Hello" -> "hELLO" |

## String Alignment

| Function | Description | Example |
|----------|-------------|---------|
| `center(str, width, fill)` | Center with fill char | `python::center(s, 20, "-")` |
| `ljust(str, width, fill)` | Left-justify, pad right | `python::ljust(s, 20, ".")` |
| `rjust(str, width, fill)` | Right-justify, pad left | `python::rjust(s, 20, ".")` |
| `zfill(str, width)` | Zero-pad numeric string | `python::zfill(s, 8)` — "42" -> "00000042" |

## Searching & Counting

| Function | Description | Example |
|----------|-------------|---------|
| `count(str, substr)` | Count non-overlapping occurrences | `n = python::count(s, "an")` |
| `startswith(str, prefix)` | Check prefix (returns 0/1) | `if ( python::startswith(s, "http") )` |
| `endswith(str, suffix)` | Check suffix (returns 0/1) | `if ( python::endswith(s, ".txt") )` |

## Character Class Tests

| Function | Description | Example |
|----------|-------------|---------|
| `isdigit(str)` | All digits? | `if ( python::isdigit(s) )` |
| `isalpha(str)` | All alphabetic? | `if ( python::isalpha(s) )` |
| `isalnum(str)` | All alphanumeric? | `if ( python::isalnum(s) )` |
| `isspace(str)` | All whitespace? | `if ( python::isspace(s) )` |

## String Manipulation

| Function | Description | Example |
|----------|-------------|---------|
| `replace(str, old, new)` | Replace all occurrences | `python::replace(s, "foo", "bar")` |

## Formatting

| Function | Description | Example |
|----------|-------------|---------|
| `format(result, fmt, args)` | Python-style formatting on the shared `std::format` engine | See below |

`format` runs the same engine as madc's `std::format` (Python's format
spec is std::format's ancestor, so the grammar is shared): `{}`
automatic and `{0}` manual indexing, format specs (`{:>8}`, `{:.2f}`,
`{:#06x}`), and `{{ }}` escaping, with each argument formatted by its
runtime value kind. Errors (malformed string, index out of range,
manual/automatic mix) render a loud inline
`[python format failed: ...]` marker — never silence.

The primary form takes a `value` result (dialect-lean); a `std::string`
result overload exists when `<string>` precedes `<ns_python>`:

```c
var args;
php::array_push(args, "World");
php::array_push(args, 42);
var result;
python::format(result, "Hello {}, the answer is {}", args);
// result: "Hello World, the answer is 42"
python::format(result, "{1:#06x} before {0:>8}", args);
// result: "0x002a before    World"
```

## Example

```c
int main()
{
    string s = "hello world";
    python::title(s);
    cout << s << endl;          // Hello World

    s = "42";
    python::zfill(s, 8);
    cout << s << endl;          // 00000042

    s = "banana";
    string sub = "an";
    int n;
    n = python::count(s, sub);
    cout << n << endl;          // 2

    return 0;
}
```
