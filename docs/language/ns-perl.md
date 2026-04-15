# perl:: Namespace

Perl-unique string and array functions. Perl's `chop`/`chomp`, `grep`, `glob`, and array manipulation functions (`push`, `pop`, `shift`, `unshift`, `split`, `join`).

## String Functions

| Function | Description | Example |
|----------|-------------|---------|
| `chop(str)` | Remove last character, return it as int | `ch = perl::chop(s)` |
| `chomp(str)` | Remove trailing newline(s), return count | `n = perl::chomp(s)` |
| `lc(str)` | Lowercase entire string | `perl::lc(s)` |
| `uc(str)` | Uppercase entire string | `perl::uc(s)` |
| `ucfirst(str)` | Capitalize first character | `perl::ucfirst(s)` |
| `lcfirst(str)` | Lowercase first character | `perl::lcfirst(s)` |
| `reverse(str)` | Reverse string in place | `perl::reverse(s)` |
| `index(str, substr)` | Find first position (-1 if missing) | `i = perl::index(s, "foo")` |
| `rindex(str, substr)` | Find last position (-1 if missing) | `i = perl::rindex(s, "foo")` |
| `length(str)` | String length | `n = perl::length(s)` |
| `substr(result, str, offset, len)` | Extract substring | `perl::substr(r, s, 2, 5)` |

## Array Functions

| Function | Description | Example |
|----------|-------------|---------|
| `split(arr, delim, str)` | Split string into array | `perl::split(a, ":", path)` |
| `join(result, sep, arr)` | Join array into string | `perl::join(s, ",", a)` |
| `push(arr, str)` | Append element | `perl::push(a, "val")` |
| `pop(result, arr)` | Remove + return last element | `perl::pop(s, a)` |
| `shift(result, arr)` | Remove + return first element | `perl::shift(s, a)` |
| `unshift(arr, str)` | Prepend element | `perl::unshift(a, "val")` |
| `scalar(arr)` | Number of elements | `n = perl::scalar(a)` |
| `grep(dest, pattern, src)` | Filter by substring match | `perl::grep(matches, "err", lines)` |
| `glob(arr, pattern)` | File globbing | `perl::glob(files, "*.mad")` |

## Key Differences from PHP

| Perl | PHP | Difference |
|------|-----|------------|
| `perl::chop(s)` | `php::chop(s)` | Perl removes last char; PHP strips trailing whitespace |
| `perl::split(a, d, s)` | `php::explode(a, d, s)` | Same operation, different name and arg order |
| `perl::join(r, s, a)` | `php::implode(r, s, a)` | Same operation, different name |
| `perl::scalar(a)` | `php::count(a)` | Same operation, Perl naming |

## Example

```c
int main()
{
    // grep: filter log lines
    array lines;
    php::array_push(lines, "INFO: all good");
    php::array_push(lines, "ERROR: disk full");
    php::array_push(lines, "INFO: started");
    php::array_push(lines, "ERROR: timeout");

    array errors;
    string pattern = "ERROR";
    perl::grep(errors, pattern, lines);
    // errors now contains the 2 ERROR lines

    // glob: find test files
    array tests;
    string glob_pat = "tests/test*.mad";
    perl::glob(tests, glob_pat);

    return 0;
}
```
