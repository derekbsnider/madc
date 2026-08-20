# python:: Namespace

Python-unique string operations: title case, case swapping, string alignment, zero-padding, character class testing, substring counting, and `format()` with `{}` placeholders.

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
| `format(result, fmt, args)` | Python-style `{}` formatting | See below |

The `format` function takes a format string with `{}` placeholders and a MadArray of values:

```c
array args;
php::array_push(args, "World");
php::array_push(args, 42);
string fmt = "Hello {}, the answer is {}";
string result;
python::format(result, fmt, args);
// result: "Hello World, the answer is 42"
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
