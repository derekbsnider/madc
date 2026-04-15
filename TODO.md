# TODO

## Medium Priority

- **Multi-return in brace-less if** — `if (x) return a, b;` (no braces) fails to parse because comma confuses the single-statement if body. Workaround: use braces `if (x) { return a, b; }`. Braced multi-return works correctly.

## Low Priority

- **`(type, type)` multi-return declaration syntax** — Currently multi-return is inferred from `return a, b;`. Add explicit `(int, string) func()` declaration for documentation and type safety.

- **Multiple return values with string types** — Current multi-return uses 8-byte slots (int64). String returns would need pointer passing via the retbuf. Works for numeric types now.

- **Phase 4: `libmadc.so` embedding API** — Decouple static globals, create public C API (`madc_create`, `madc_exec_file`, `madc_exec_string`, `madc_destroy`), build as shared library.

## Completed

- ~~Escape sequences in string literals~~ (c90acff)
- ~~`[]` subscript operator~~ (c90acff)
- ~~`:=` short variable declaration~~ (7027d4c)
- ~~`[&]` lambda capture by reference~~ (4fa5126)
- ~~`madc::` namespace~~ — `madc::array` + regex functions
- ~~`std::` namespace scoping~~ — `std::vector<T>`, `std::map<K,V>`, `std::set<T>`, `std::list<T>`, `std::cin`
- ~~Register-only iterator~~ — numeric foreach element variables use `vfREGISTER`
- ~~Fix asmjit deprecation warnings~~ — ~70 call sites migrated
- ~~`switch` statement~~ — C-style with fall-through and break (deb3578)
- ~~`>>` input operator / `cin`~~ — reads string, int, double from stdin (1feb665)
- ~~Class methods~~ — hidden `__this` pointer, member access, name mangling (f375944)
- ~~Regex support~~ — `madc::regex_match/search/replace`, upgraded `perl::grep/split` (2aabea0)
- ~~Multiple return values~~ — `q, r := divide(17, 5)` with hidden `__retbuf` (b1c0e86, 8d07f44)
- ~~Ternary operator~~ — `condition ? true_expr : false_expr` with stack-slot merge (f64cf18)
- ~~Multi-return conditional crash~~ — skip cleanup for multi-return paths (8d07f44)
