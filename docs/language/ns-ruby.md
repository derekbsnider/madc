# ruby:: Namespace

Ruby-unique string and array operations: character transliteration, consecutive duplicate squeezing, character splitting, array rotation, and more.

## Dialect (lean) forms — the primary surface

Available in every madc TU with zero includes (dialect-lean,
2026-08-21). These are Ruby's NON-BANG methods — they return a NEW
string. Text returns are ring-lifetime `const char *`; subjects can be
a `value` or a `const char *`: `ruby::squeeze(s)`, `capitalize(s)`,
`tr(s, from, to)`, `delete(s, chars)`, `gsub(s, pat, rep)`,
`sub(s, pat, rep)`. The `std::string`-flavored forms are C++-interop
conveniences, declared only when `<string>` precedes `<ns_ruby>`.

## String Functions

| Function | Description | Example |
|----------|-------------|---------|
| `squeeze(str)` | Collapse consecutive duplicate chars | `ruby::squeeze(s)` — "aaabbc" -> "abc" |
| `tr(str, from, to)` | Transliterate characters | `ruby::tr(s, "aeiou", "*")` — "hello" -> "h*ll*" |
| `capitalize(str)` | Uppercase first, lowercase rest | `ruby::capitalize(s)` — "hELLO" -> "Hello" |
| `delete(str, chars)` | Remove all specified characters | `ruby::delete(s, "lo")` — "hello" -> "he" |
| `count(str, chars)` | Count occurrences of any listed char | `n = ruby::count(s, "aeiou")` |
| `include(str, substr)` | Check if string contains substr | `if ( ruby::include(s, "world") )` |
| `gsub(str, pattern, repl)` | Global substitution (all matches) | `ruby::gsub(s, "foo", "bar")` |
| `sub(str, pattern, repl)` | Substitute first occurrence only | `ruby::sub(s, "foo", "bar")` |

## Array Functions

| Function | Description | Example |
|----------|-------------|---------|
| `chars(arr, str)` | Split string into individual chars | `ruby::chars(a, "hello")` |
| `rotate(arr, n)` | Rotate elements by n positions | `ruby::rotate(a, 2)` — [a,b,c,d] -> [c,d,a,b] |
| `compact(arr)` | Remove empty string entries | `ruby::compact(a)` |
| `flatten(arr, str)` | Split string by whitespace into array | `ruby::flatten(a, "one two three")` |

## Key Differences

| Ruby | Perl | PHP | Difference |
|------|------|-----|------------|
| `ruby::squeeze(s)` | — | — | Unique to Ruby |
| `ruby::tr(s,f,t)` | — | — | Unique to Ruby (like Unix `tr`) |
| `ruby::gsub(s,p,r)` | — | `php::str_replace(p,r,s)` | Same operation, different arg order |
| `ruby::sub(s,p,r)` | — | — | First-occurrence-only replace |
| `ruby::chars(a,s)` | — | — | Unique to Ruby |
| `ruby::rotate(a,n)` | — | — | Unique to Ruby |

## Example

```c
int main()
{
    // transliterate vowels to numbers
    string s = "hello world";
    string from = "aeiou";
    string to = "12345";
    ruby::tr(s, from, to);
    cout << s << endl;          // h2ll4 w4rld

    // squeeze repeated chars
    string dups = "aabbccddee";
    ruby::squeeze(dups);
    cout << dups << endl;       // abcde

    // split into chars and rotate
    array chars;
    ruby::chars(chars, "abcde");
    ruby::rotate(chars, 2);
    string result;
    string empty = "";
    php::implode(result, empty, chars);
    cout << result << endl;     // cdeab

    return 0;
}
```
