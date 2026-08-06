# rust:: Namespace

`rust::` provides Rust-inspired helper functions over madc's existing
`string` and `array` types. This is namespace sugar only; it does not
add Rust ownership, borrowing, lifetimes, enums, traits, or pattern
matching semantics.

## String helpers

- `rust::contains(str, needle)` — returns `1` if `needle` is present
- `rust::starts_with(str, prefix)` — returns `1` if `str` starts with `prefix`
- `rust::ends_with(str, suffix)` — returns `1` if `str` ends with `suffix`
- `rust::trim(str)` — trims both ends in place
- `rust::trim_start(str)` — trims the left side in place
- `rust::trim_end(str)` — trims the right side in place
- `rust::replace(str, from, to)` — replaces all occurrences in place
- `rust::repeat(str, count)` — repeats the string in place
- `rust::len(str)` — returns string length
- `rust::is_empty(str)` — returns `1` if empty, else `0`

## Array helpers

- `rust::split(arr, str, delim)` — splits `str` by `delim` into `arr`
- `rust::split_whitespace(arr, str)` — splits on runs of whitespace
- `rust::join(result, arr, sep)` — joins array elements into `result`
- `rust::first(result, arr)` — copies the first element into `result`
- `rust::last(result, arr)` — copies the last element into `result`
- `rust::get(result, arr, idx)` — copies the indexed element into `result`
- `rust::push(arr, value)` — appends a string value
- `rust::pop(result, arr)` — pops the last element into `result`

## Example

```c
string s = "  hello rust  ";
array parts;
string out;

rust::trim(s);
rust::split(parts, s, " ");
rust::join(out, parts, "-");
cout << rust::contains(out, "rust") << endl;
```

## `rust::match`

The `rust::match` statement form (multi-pattern, no-fall-through
`switch` with a `_` wildcard arm) is implemented — see
[rust-match.md](rust-match.md).

## Planned, Not Yet Implemented

- `rust::if let` syntax
- ownership / borrowing / lifetimes
- `Option<T>` / `Result<T, E>` as first-class types
