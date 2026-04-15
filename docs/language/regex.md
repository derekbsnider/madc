# Regex Support

Regular expressions via `std::regex`, available through the `madc::` namespace and upgraded `perl::` functions.

## madc:: Functions

### regex_match -- full string match

```c
int m = madc::regex_match(str, pattern);
```

Returns 1 if the entire string matches the pattern, 0 otherwise.

### regex_search -- partial match

```c
int found = madc::regex_search(str, pattern);
```

Returns 1 if the pattern matches anywhere in the string, 0 otherwise.

### regex_replace -- substitution

```c
string result;
madc::regex_replace(result, source, pattern, replacement);
```

Replaces all occurrences of the pattern in `source` and stores the result. The original string is not modified.

## Upgraded perl:: Functions

### perl::grep with regex

`perl::grep` now uses `std::regex_search` internally. If the pattern is not a valid regex, it falls back to substring matching.

```c
array words;
php::explode(words, ",", "apple,banana,cherry,avocado");
array matches;
perl::grep(matches, "^a", words);
// matches: apple, avocado
```

### perl::split with regex

`perl::split` now uses `std::sregex_token_iterator`. If the pattern is not a valid regex, it falls back to literal delimiter splitting.

```c
array parts;
perl::split(parts, ":+", "one:two::three:::four");
// parts: one, two, three, four
```

## Example

```c
string s = "hello123";
int m = madc::regex_match(s, "hello[0-9]+");
cout << m << endl;                              // 1

string s2 = "foo bar baz";
int found = madc::regex_search(s2, "bar");
cout << found << endl;                          // 1

string result;
madc::regex_replace(result, "The quick brown fox", "quick|brown", "slow");
cout << result << endl;                         // The slow slow fox
```

## Error Handling

Invalid regex patterns are caught internally and fall back gracefully -- no crash or exception propagates to the Mad-C program.

## Files

- `src/ns_perl.cpp` -- upgraded `perl::grep`, `perl::split` with regex support
- `src/compiler.cpp` -- `madc::regex_match`, `madc::regex_search`, `madc::regex_replace` bindings
