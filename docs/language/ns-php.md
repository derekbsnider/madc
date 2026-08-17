# php:: Namespace

PHP-unique string and array functions. String functions modify in place and return the same pointer. Array functions use the `MadValue`-based `array` type internally for mixed-type storage.

## String Functions

| Function | Description | Example |
|----------|-------------|---------|
| `trim(str)` | Strip whitespace from both ends | `php::trim(s)` |
| `ltrim(str)` | Strip whitespace from left | `php::ltrim(s)` |
| `rtrim(str)` | Strip whitespace from right | `php::rtrim(s)` |
| `chop(str)` | Alias for `rtrim` | `php::chop(s)` |
| `ucfirst(str)` | Capitalize first character | `php::ucfirst(s)` |
| `lcfirst(str)` | Lowercase first character | `php::lcfirst(s)` |
| `str_repeat(str, n)` | Repeat string n times | `php::str_repeat(s, 3)` |
| `str_replace(search, replace, subject)` | Replace all occurrences | `php::str_replace(old, new, s)` |
| `str_pad(str, length, pad_str)` | Pad string to length | `php::str_pad(s, 20, ".")` |
| `str_word_count(str)` | Count words | `n = php::str_word_count(s)` |
| `nl2br(str)` | Convert newlines to `<br>` | `php::nl2br(s)` |
| `str_rot13(str)` | ROT13 encoding | `php::str_rot13(s)` |
| `chunk_split(str, len, sep)` | Insert separator every N chars | `php::chunk_split(s, 4, "-")` |
| `number_format(result, number, sep)` | Format with thousands separator | `php::number_format(s, 1234567, ",")` |
| `wordwrap(str, width, break)` | Wrap text at width | `php::wordwrap(s, 72, "\n")` |

## Array Functions

Arrays use the `array` data type, which stores mixed-type elements (MadValue).

| Function | Description | Example |
|----------|-------------|---------|
| `explode(arr, delim, str)` | Split string into array | `php::explode(a, ",", csv)` |
| `implode(result, glue, arr)` | Join array into string | `php::implode(s, ",", a)` |
| `count(arr)` | Number of elements | `n = php::count(a)` |
| `array_push(arr, str)` | Append string | `php::array_push(a, "val")` |
| `array_push_int(arr, int)` | Append integer | `php::array_push_int(a, 42)` |
| `array_push_array(arr, nested)` | Append an array as a nested row | `php::array_push_array(rows, row)` |
| `array_pop(result, arr)` | Remove + return last element | `php::array_pop(s, a)` |
| `array_get(result, arr, index)` | Get element as string | `php::array_get(s, a, 0)` |
| `array_get_int(arr, index)` | Get element as int | `n = php::array_get_int(a, 0)` |
| `array_get_cstr(arr, index)` | Get element as `const char *` | `puts(php::array_get_cstr(a, 0))` |
| `array_shift(result, arr)` | Remove + return first element | `php::array_shift(s, a)` |
| `array_unshift(arr, str)` | Prepend element | `php::array_unshift(a, "first")` |
| `array_reverse(arr)` | Reverse in place | `php::array_reverse(a)` |
| `array_unique(arr)` | Remove duplicates | `php::array_unique(a)` |
| `array_search(needle, arr)` | Find index (-1 if missing) | `i = php::array_search(s, a)` |
| `array_slice(dest, src, off, len)` | Extract sub-array | `php::array_slice(b, a, 1, 3)` |
| `array_merge(dest, src)` | Append src to dest | `php::array_merge(a, b)` |
| `array_column(dest, src, idx)` | Extract element `idx` from each nested row | `php::array_column(names, rows, 0)` |
| `in_array(needle, arr)` | Check if value exists | `if ( php::in_array(s, a) )` |
| `sort(arr)` | Sort ascending | `php::sort(a)` |
| `rsort(arr)` | Sort descending | `php::rsort(a)` |

## Example

```c
int main()
{
    string csv = "alice,bob,charlie";
    string delim = ",";
    array names;

    php::explode(names, delim, csv);
    php::sort(names);

    string joined;
    string pipe = " | ";
    php::implode(joined, pipe, names);
    cout << joined << endl;  // alice | bob | charlie

    return 0;
}
```

## Nested Rows Example

`array` elements can themselves be arrays — `array_push_array` builds a
table of rows, and `array_column` pulls one column back out:

```c
int main()
{
    array rows;
    array alice;
    array bob;
    php::array_push(alice, "alice");
    php::array_push_int(alice, 30);
    php::array_push(bob, "bob");
    php::array_push_int(bob, 25);
    php::array_push_array(rows, alice);
    php::array_push_array(rows, bob);

    array names;
    php::array_column(names, rows, 0);   // element 0 of every row
    string joined;
    string sep = ", ";
    php::implode(joined, sep, names);
    cout << joined << endl;              // alice, bob

    return 0;
}
```

## Dump Functions — `print_r` and `var_dump` over ANY madc type

`php::print_r(v)` and `php::var_dump(v...)` render **any** madc value the way a
PHP developer expects PHP to render it: a struct prints like a PHP object, a
fixed array like a PHP array, a `char *` or `char[]` like a string. They are not
limited to `array` / `value` — the compiler generates the dumper for whatever
type the argument has, so a `struct`, a `class` with private members, a `union`,
a bit-field and a nested array all work.

| Function | Description | Example |
|----------|-------------|---------|
| `print_r(v)` | PHP's `print_r` — the value, framed for humans | `php::print_r(pt)` |
| `var_dump(v, ...)` | PHP's `var_dump`, variadic, with the REAL C/C++/madc type of each value | `php::var_dump(i, s)` |

```c
#include <ns_php>

struct Point { int x; int y; };

int main()
{
    Point pt;
    pt.x = 1;
    pt.y = 2;
    php::print_r(pt);
    php::var_dump(pt);
    return 0;
}
```

```
Point Object
(
    [x] => 1
    [y] => 2
)
struct Point(2) {
  ["x"]=>
  int(1)
  ["y"]=>
  int(2)
}
```

`print_r` output is byte-identical to PHP's for every shape PHP can express —
the 4-space entry indent, the 8-space step of a nested `(`, the blank line after
a nested block, `[prot:protected]`, `[priv:Class:private]`, `1` for `true` and
nothing at all for `false`. A double carries PHP's 14 significant digits,
including PHP's `1.0E+25` mantissa where C's `%G` would print `1E+25`.

`var_dump` keeps PHP's frame (2-space indent, the key line and value line
separate, `}` with no trailing blank line) and makes exactly one deliberate
change: it names the **real** type instead of simulating PHP's.

| value | PHP | madc |
|---|---|---|
| `42` (`int`) | `int(42)` | `int(42)` |
| `42L` | `int(42)` | `long(42)` |
| `4000000000u` | `int(4000000000)` | `unsigned int(4000000000)` |
| `3.5` | `float(3.5)` | `double(3.5)` |
| `1.5f` | `float(1.5)` | `float(1.5)` |
| `"hi"` | `string(2) "hi"` | `char *(2) "hi"` |
| `char name[8]` = "hi" | — | `char[8](2) "hi"` |
| a `Point` | `object(Point)#1 (2)` | `struct Point(2)` |
| `int v[3]` | `array(3)` | `int[3](3)` |
| a `std::vector<int>` | `array(3)` | `std::vector<int32_t,std::allocator<int32_t>>(3)` |
| a `std::string` | `string(2) "hi"` | `std::__cxx11::basic_string<char,...>(2) "hi"` |

The type word is the CANONICAL type, not the typedef the source wrote — the same
thing `typeid` reports in g++ — so a `size_t` shows as `unsigned long`. An
aggregate is always spelled `struct X` (or `union X`): madc promotes a plain
struct to a class internally when it earns class-hood, so `class` would be a
claim about your source that the compiler cannot support.

### What the dump shows for C shapes PHP does not have

- **`union`** — every member's interpretation of the same storage, since C
  offers no active-member truth. `u.i = 0x41424344` prints both
  `[i] => 1094861636` and `[c] => DCBA` on a little-endian target.
- **anonymous `struct` / `union` members** — named directly, as C names them.
- **bit-fields** — their value. (The width belongs to the type word, not the
  value.)
- **`char` / `unsigned char`** — one character of text, matching
  `cout << (char)65` and PHP's `chr(65)`.

### Containers

A class that offers `size()` and `operator[](integral)` — `std::vector`,
`std::array`, `std::string`, and any user class shaped the same way — is a
POSITIONAL SEQUENCE, and prints as one. That is a structural test, not a list of
blessed container names: nothing here matches `c_str`, `length` or `vector` by
name.

- **A sequence whose element is a character type is TEXT**, which is how
  `std::string` prints as its contents rather than as an array of small integers
  — and why `std::vector<char>` does the same.
- **Any other sequence is a PHP array**, keyed `[0..size()-1]`.
- The count is read from `size()` ONCE, so the printed count and the elements
  always agree.
- An ASSOCIATIVE container (`std::map`, `std::set`) is not a positional
  sequence — its `operator[]` takes a key, not a position — so it falls back to
  showing its members. Rendering `[key] => value` needs the `begin()`/`end()`
  protocol, which madc does not implement yet.
- A class that looks positional but whose element type has no dumper yet (a
  `Matrix::operator[]` returning a row pointer) also falls back to its members:
  the sequence rendering is an enhancement and never removes information.

`var_dump` names a container by its canonical C++ spelling, which for a template
instantiation is long (`std::vector<int32_t,std::allocator<int32_t>>`) and
depends on the standard-library flavor. `print_r` is the readable form.

### Limits

- These two are **compiler intrinsics**: they are declared in `<ns_php>` and
  defined nowhere, so they cannot be called from a C or C++ host that merely
  links `libmadc` — there is no symbol to call. That is by design.
- An aggregate argument must be a variable or a member selection. A
  struct-returning call would have to be re-evaluated once per field, so it is
  refused out loud instead.
- A member inherited from a base and **shadowed** by a same-named member in the
  derived class is skipped: the emitted struct renames the hidden one and no
  reader can address it.
- Types still to come: pointers (followed, with PHP's `*RECURSION*` for a
  cycle), enums (by enumerator name), associative containers, multidimensional
  arrays, and `array` / `value` itself. Each is refused by name until then —
  never guessed at.
- `print_r($v, true)`'s return form is not implemented yet; `print_r` currently
  always prints.
