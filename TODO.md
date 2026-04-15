# TODO

## Low Priority

- **Multiple return values** — Go-style `val, err := func()`. High effort, touches calling conventions and the `regdefp_t` system.

- **`switch` statement** — Keyword exists but no compile support.

- **`>>` input operator / `cin`** — Currently `>>` is bitwise right-shift only.

- **Class methods** — Parser detects them but no `this` pointer compilation yet.

- **Regex support** — `grep`/`split` currently use substring match, not regex.

## Completed

- ~~Escape sequences in string literals~~ (c90acff)
- ~~`[]` subscript operator~~ (c90acff)
- ~~`:=` short variable declaration~~ (7027d4c)
- ~~`[&]` lambda capture by reference~~ (4fa5126)
- ~~`madc::` namespace~~ — `madc::array` works alongside bare `array`
- ~~`std::` namespace scoping~~ — `std::vector<T>`, `std::map<K,V>`, `std::set<T>`, `std::list<T>` all work
- ~~Register-only iterator~~ — numeric foreach element variables use `vfREGISTER`
- ~~Fix asmjit deprecation warnings~~ — FuncSignatureT→FuncSignature::build, size()→x86RmSize()
