# Namespace Precedence — `prefer`

Unqualified names that ordinary scope lookup does not resolve fall through
to an ordered namespace walk. This is a madc-dialect feature; standards
modes (`--std=c*` / `--std=c++*`) have no preference walk.

## The default order

madc mode ships a default precedence, so most scripts never need a
directive at all:

```
c, std, php, perl, python, ruby, js, rust, madc
```

`c` means the normal madc/C lexical and global lookup path — it is first,
so anything you define (or libc provides) always wins over a namespace
function of the same name. Only namespaces whose headers are loaded can
match; with auto-include, mentioning `php::` anywhere (or naming a
namespace in a `prefer` directive) is enough to load it.

## Overriding the order

Two equivalent spellings update the order from that point forward:

```text
prefer rust, std, c;
```

```text
#pragma prefer rust, std, c
```

An explicit directive **replaces the default wholly** — it is not merged.
Include `std` and `c` in your list unless you intend to hide them from
unqualified lookup (qualified names like `std::endl` and explicit
`php::trim` always work regardless of the preference order).

## Example

```c
string &trim(string &s)      // a user function that shadows the utilities
{
    s = "user";
    return s;
}

string a = "  x  ";
trim(a);                     // default order: "c" first — user trim wins
cout << "[" << a << "]" << endl;

prefer rust, std, c;         // rust now outranks the user function
string b = "  y  ";
trim(b);
cout << "[" << b << "]" << endl;
```

Output:

```
[user]
[y]
```

## Notes

- `prefer` affects unqualified identifier resolution only; `ns::member`
  stays explicit.
- The directive applies from its position forward in the file.
- Namespaced special forms such as `rust::match` build on the same
  precedence model.
