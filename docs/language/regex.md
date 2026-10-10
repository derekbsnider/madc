# Regex Support

madc has ONE regex engine, [PCRE2](https://github.com/PCRE2Project/pcre2) —
the engine behind PHP's `preg_*` — reached through the `madc::` namespace and
the regex-aware `perl::` functions. The same engine runs on every platform
(Linux links the distribution's `libpcre2-8`; the macOS and Windows packages
carry a pinned static PCRE2 10.42), so a pattern means the same thing
everywhere.

## Syntax

PCRE2's own — Perl / PHP syntax, written WITHOUT delimiters (`"\\d+"`, not
`"/\\d+/"`):

- unescaped `+ ? | ( ) { }`; `\d \w \s \b`; character classes `[a-z]`
- lazy quantifiers `+? *? ??`
- lookahead `(?=…)` `(?!…)` and lookbehind `(?<=…)` `(?<!…)`
- named groups `(?<name>…)`, inline flags `(?i)` `(?m)` `(?s)` `(?x)`

Patterns and subjects are **UTF-8**: `.` matches one code point (`é` is one,
not two bytes), and a subject holding invalid UTF-8 never faults — those bytes
simply cannot match a UTF construct. A backslash in a madc string literal is
itself escaped: the pattern `\d+` is written `"\\d+"`.

## madc:: Functions

| Function | Meaning |
|----------|---------|
| `madc::regex_match(str, pattern)` | true if the **entire** string matches |
| `madc::regex_search(str, pattern)` | true if the pattern matches **anywhere** |
| `madc::regex_replace(out, source, pattern, replacement)` | `out` = `source` with **every** match replaced; `source` unchanged |

A replacement names `$0` (the whole match), `$1` / `${1}` (a group) and `$$`
(a dollar sign); a group that did not take part in the match is empty.

```c
println("{}", madc::regex_match("hello123", "hello[0-9]+"));	// true
println("{}", madc::regex_search("price: 10USD", "\\d+(?=USD)"));	// true

var out;
madc::regex_replace(out, "The quick brown fox", "quick|brown", "slow");
println("{}", out);	// The slow slow fox
madc::regex_replace(out, "me@host", "(\\w+)@(\\w+)", "$2 at ${1}");
println("{}", out);	// host at me
```

The functions take `const char *` (a `var` passes its text); a program that
includes `<string>` also gets `std::string` forms, with `out` a
`std::string &`.

## Regex-aware perl:: Functions

| Function | Meaning |
|----------|---------|
| `perl::grep(dest, pattern, src)` | `dest` = the string elements of `src` the pattern matches (Perl's `grep { /p/ } @src`) |
| `perl::split(dest, pattern, str)` | `dest` = Perl's `split(/p/, str)` fields |

`perl::split` follows Perl: a match at the very start keeps an empty leading
field, TRAILING empty fields are dropped, and an empty pattern splits the
string into its code points.

```c
array parts;
perl::split(parts, ":+", "one:two::three:::four");
println("{}", perl::join("|", parts));	// one|two|three|four
perl::split(parts, ":", ":a:b::");
println("{}", perl::join("|", parts));	// |a|b
perl::split(parts, "", "abc");
println("{}", perl::join("|", parts));	// a|b|c
```

## An invalid pattern is refused

A pattern that does not compile is never quietly answered. The function
prints one line on stderr naming itself, the pattern, PCRE2's reason and the
byte offset where it stopped, and returns a neutral answer:

```
madc::regex_search: invalid pattern "a(b": missing closing parenthesis at offset 3
```

| Function | Answer on a refused pattern |
|----------|-----------------------------|
| `regex_match` / `regex_search` | false |
| `regex_replace` | `out` = the source, unchanged |
| `perl::grep` / `perl::split` | `dest` = an empty array |

A malformed replacement (`$9` with no ninth group) is refused the same way by
`regex_replace` (`invalid replacement "$9": …`, `out` = the source).

## Thread safety

Each call compiles its own pattern and keeps no state between calls: the
functions are safe to call concurrently (the C++ standard library's
convention — distinct objects are independent).

## Files

- `include/madc_regex.h`, `src/madc_regex.cpp` — `madc::regex`, the one
  engine (the only includer of `pcre2.h`; gate:
  `scripts/check-one-regex-engine.sh`)
- `src/ns_madc.cpp` — `madc::regex_match` / `regex_search` / `regex_replace`
- `src/ns_perl.cpp` — `perl::grep`, `perl::split`
- `scripts/stage_pcre2.sh` — the macOS / Windows static PCRE2 stage
