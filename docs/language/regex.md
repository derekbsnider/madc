# Regex Support

Regular expressions via `std::regex`, available through the `madc::`
namespace and regex-aware `perl::` functions.

## madc:: Functions

| Function | Meaning |
|----------|---------|
| `madc::regex_match(str, pattern)` | 1 if the **entire** string matches, else 0 |
| `madc::regex_search(str, pattern)` | 1 if the pattern matches **anywhere**, else 0 |
| `madc::regex_replace(result, source, pattern, replacement)` | replace all occurrences into `result`; `source` unchanged |

```c
string s = "hello123";
cout << madc::regex_match(s, "hello[0-9]+") << endl;

string s2 = "foo bar baz";
cout << madc::regex_search(s2, "bar") << endl;

string result;
madc::regex_replace(result, "The quick brown fox", "quick|brown", "slow");
cout << result << endl;
```

Output:

```
1
1
The slow slow fox
```

## Regex-aware perl:: Functions

`perl::grep` uses `std::regex_search` internally; `perl::split` uses
`std::sregex_token_iterator`. Either falls back to plain substring /
literal-delimiter behavior when the pattern is not a valid regex.

```c
array words;
php::explode(words, ",", "apple,banana,cherry,avocado");
array matches;
perl::grep(matches, "^a", words);
cout << perl::scalar(matches) << endl;

array parts;
perl::split(parts, ":+", "one:two::three:::four");
cout << perl::scalar(parts) << endl;
```

Output:

```
2
4
```

## Error Handling

Invalid regex patterns are caught internally and fall back gracefully —
no exception propagates into the madc program.

## Files

- `src/ns_perl.cpp` — regex-aware `perl::grep`, `perl::split`
- `src/parser.cpp` — `madc::regex_match` / `regex_search` /
  `regex_replace` registration and bindings
