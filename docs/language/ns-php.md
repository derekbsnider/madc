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
| a `std::vector<int>` | `array(3)` | `std::vector<int>(3)` |
| a `std::string` | `string(2) "hi"` | `std::string(2) "hi"` |

The type word is the name the SOURCE gives the type. For a template
instantiation that is the alias the standard library declared —
`typedef basic_string<char> string;` makes the word `std::string`, not
`std::__cxx11::basic_string<char,std::char_traits<char>,std::allocator<char>>` —
found by looking the type up in madc's own type-name tables, so it is stable
across standard-library flavors. A container adds its ELEMENT type and drops the
defaulted allocator and traits: `std::vector<std::string>`. For a plain scalar the
word is the canonical type rather than a typedef of it (a `size_t` shows as
`unsigned long`, the same thing `typeid` reports in g++). An
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
- A class that looks positional but whose element type has no dumper yet (a
  `Matrix::operator[]` returning a row pointer) falls back to its members: the
  sequence rendering is an enhancement and never removes information.

A container with **no position** — `std::map`, `std::set`, `std::list` — prints
too, through C++'s own iterator protocol: `begin()` / `end()` yielding an iterator
with `operator*` and prefix `operator++` (a class iterator or a raw pointer one),
plus `size()`. That is the same structural test the range-based `for` uses, so
whatever prints here also iterates there.

- **A KEYED container prints `[key] => value`**, never the `std::pair` its
  elements actually are. Keyed means the container names a `mapped_type` — the
  standard library's own signal — so a `std::map` is keyed and a `std::set` and a
  `std::list` are not.
- **A set and a list print POSITIONALLY**, keyed `[0..size()-1]`, exactly like a
  vector. PHP has no set, and a list of values is a list.
- **The key is rendered by the same walk as any other value**, so an integral key
  prints as a number and a `std::string` key as its text. `var_dump` quotes a
  string key (`["b"]=>`) and leaves an integral one bare (`[3]=>`), which is what
  PHP does.
- **A `std::map` and a `std::set` are key-ORDERED**, so entries appear in key
  order rather than insertion order. That is the container's semantics, not a
  rendering choice.
- The loop is **counted off `size()`**. A container with `begin()`/`end()` and no
  `size()` has no bound this walk can use and is refused by name, saying so.

`var_dump` names a container the way you would write it: `std::vector<int>`,
`std::map<std::string,int>`, `std::list<int>`, `std::string`. A `std::array`'s
extent is not in the word — it is the count in parentheses.

### Pointers

PHP has no pointers, so a pointer is **followed**: the pointee is printed, at the
SAME depth, because an indirection is not a nesting level. A null pointer prints
as PHP's null — nothing at all under `print_r`, `NULL` under `var_dump`. A `char *`
is PHP's string and stays text.

- **A cycle is marked, not chased.** A ring, a self-pointer and a mutual ring each
  end in PHP's `*RECURSION*`, carrying the word of the frame it replaces.
- **A value reachable TWICE without a cycle prints IN FULL both times**, which is
  what PHP does. The test is "is this already on the path I am printing", not
  "have I ever seen it".
- A long list prints in full — thousands of nodes — because the pointee walk is a
  generated function and the recursion is a real call.

### Enums

An enum prints as **its enumerator's name plus its value**, which is PHP 8.1's own
enum shape:

```
Color Enum:unsigned int          # print_r
(
    [name] => GREEN
    [value] => 1
)

enum(Color::GREEN)               # var_dump
```

The head word carries the REAL backing type. A value naming no enumerator —
`(Color)7`, legal C with no PHP form — shows an empty `[name]`, and `var_dump`
prints `enum Color(7)` rather than inventing a case. Scoped, class-nested and
fixed-base enums all work; duplicate enumerator values resolve to the first name,
which is what a debugger shows.

### `array` and `value`

madc's own dynamic carrier prints across all nine of its kinds, with the nesting
and the `*RECURSION*` marker PHP gives an array of arrays. `var_dump` names the
KIND rather than a storage type — `integer(42)`, `real(3.5)`, `object(3) {` —
because a dynamically typed slot's real type IS its kind: `long(42)` would be true
of the payload and silent about the slot, and no C word distinguishes `string` from
`bytes` or `array` from `object`. `null` keeps PHP's `NULL`.

### Capturing the output

`print_r($v, true)` returns the text instead of printing it, exactly as PHP's
second parameter does:

```c
value s = php::print_r(v, true);
printf("%s", s.c_str());
```

The return is `string|true` in PHP and a `value` here: the text when the flag is
set, boolean `true` when it is not. The flag may be a runtime expression — one
walk serves both, because the output sink is a parameter rather than a compile-time
choice.

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
- A type with no dumper is **refused by name** — `no dumper for type 'X' yet`, or
  for a container `no dumper for container 'X' yet: <which piece is missing>` —
  and never guessed at. A `void *`, a function pointer and a pointer-to-member are
  addresses rather than handles on a value, so they are refused; so is a container
  with `begin()`/`end()` and no `size()`.
- **`for (auto &x : m)`** over one of these containers does not work yet: the
  range-for's element type is not deduced from `auto`. Name the type —
  `for (std::pair<const int,int> &kv : m)` — and it does.
