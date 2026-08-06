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
