# `prefer` Directive

madc supports two equivalent forms for namespace precedence:

```c
prefer rust, php, c;
```

```c
#pragma prefer rust, php, c
```

Both forms update the parser's lookup order from that point forward in
the file. `c` means the normal madc/C lexical and global lookup path.

## Example

```c
int64_t len(string s)
{
    return 99;
}

int main()
{
    string s = "hello";

    cout << len(s) << endl; // 99

    prefer rust, c;
    cout << len(s) << endl; // 5 (rust::len wins)
}
```

## Notes

- `prefer` affects unqualified identifier resolution.
- Explicit `ns::member` always stays explicit.
- `prefer` is intended as the long-term canonical form.
- `#pragma prefer ...` is a supported alias.
- Future namespaced special forms such as `rust::match` can build on the
  same precedence model.
