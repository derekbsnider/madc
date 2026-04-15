# TODO

## Medium Priority

- **Multi-return in brace-less if** — `if (x) return a, b;` (no braces) fails to parse because comma confuses the single-statement if body. Workaround: use braces `if (x) { return a, b; }`. Braced multi-return works correctly.

## Low Priority

- **`(type, type)` multi-return declaration syntax** — Currently multi-return is inferred from `return a, b;`. Add explicit `(int, string) func()` declaration for documentation and type safety.

- **Multiple return values with string types** — Current multi-return uses 8-byte slots (int64). String returns would need pointer passing via the retbuf. Works for numeric types now.

## Completed

- ~~Escape sequences in string literals~~ (c90acff)
- ~~`[]` subscript operator~~ (c90acff)
- ~~`:=` short variable declaration~~ (7027d4c)
- ~~`[&]` lambda capture by reference~~ (4fa5126)
- ~~`madc::` namespace~~ — `madc::array` works alongside bare `array`
- ~~`std::` namespace scoping~~ — `std::vector<T>`, `std::map<K,V>`, `std::set<T>`, `std::list<T>` all work
- ~~Register-only iterator~~ — numeric foreach element variables use `vfREGISTER`
- ~~Fix asmjit deprecation warnings~~ — FuncSignatureT→FuncSignature::build, size()→x86RmSize()
- ~~`switch` statement~~ — C-style with fall-through and break
- ~~`>>` input operator / `cin`~~ — reads string, int, double from stdin
- ~~Class methods~~ — hidden `__this` pointer, member access through `this`
- ~~Regex support~~ — `madc::regex_match/search/replace`, upgraded `perl::grep/split`
- ~~Multiple return values~~ — `q, r := divide(17, 5)` with hidden `__retbuf`
