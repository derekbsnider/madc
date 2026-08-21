# rust:: Namespace

`rust::` provides Rust-inspired helper functions over madc's existing
`string` and `array` types. This is namespace sugar only; it does not
add Rust ownership, borrowing, lifetimes, enums, traits, or pattern
matching semantics.

## String helpers

- `rust::contains(str, needle)` — returns `1` if `needle` is present
- `rust::starts_with(str, prefix)` — returns `1` if `str` starts with `prefix`
- `rust::ends_with(str, suffix)` — returns `1` if `str` ends with `suffix`
- `rust::trim(str)` — returns the trimmed string (lean primary; the
  guarded `std::string` form trims in place)
- `rust::trim_start(str)` — returns the left-trimmed string
- `rust::trim_end(str)` — returns the right-trimmed string
- `rust::replace(str, from, to)` — returns the string with all
  occurrences replaced
- `rust::repeat(str, count)` — returns the string repeated
- `rust::len(str)` — returns string length
- `rust::is_empty(str)` — returns `1` if empty, else `0`

Text returns are ring-lifetime `const char *` (dialect-lean,
2026-08-21); subjects can be a `value` or a `const char *`. The guarded
`std::string` forms (declared only when `<string>` precedes `<ns_rust>`)
keep the historical in-place / out-param shapes for C++ interop.

## Array helpers

- `rust::split(arr, str, delim)` — splits `str` by `delim` into `arr`
- `rust::split_whitespace(arr, str)` — splits on runs of whitespace
- `rust::join(arr, sep)` — returns the joined text
- `rust::first(out, arr)` — copies the first element into the `value`
  out-param (null when empty; Rust's `Option`)
- `rust::last(out, arr)` — copies the last element
- `rust::get(out, arr, idx)` — copies the indexed element
- `rust::push(arr, value)` — appends a string value
- `rust::pop(out, arr)` — moves the last element out (null when empty)

## Example

```c
array parts;

rust::split_whitespace(parts, rust::trim("  hello rust  "));
var joined = rust::join(parts, "-");
println("{} {}", joined, rust::contains(joined, "rust"));
```

## `rust::match`

The `rust::match` statement form (multi-pattern, no-fall-through
`switch` with a `_` wildcard arm) is implemented — see
[rust-match.md](rust-match.md).

## Planned, Not Yet Implemented

- `rust::if let` syntax
- ownership / borrowing / lifetimes
- `Option<T>` / `Result<T, E>` as first-class types
