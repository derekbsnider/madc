# Value-First madc Code

Applies to ALL madc-dialect code written in this repo: examples,
showcase programs, docs samples, and new tests.

- Prefer `var` / `madc::value` over `std::string`. Use `std::string`
  only when the point of the code IS C++/libstdc++ interop.
- Output goes through `std::print` / `std::println` / `std::format`
  (always-included madc intrinsics). Never `std::cout` / iostream in
  madc-dialect code. `printf` only in low-level plumbing tests.
- A string-kind value must be usable like a `std::string`. A missing
  string capability on the carrier is a FEEDER GAP: fix it at the
  carrier (ddARRAY script-method registry in `add_array_methods()` +
  `madarray_*` runtime entries; `php::` parity functions for PHP-shaped
  operations) — never fall back to `std::string` to dodge the gap.
- Until value-by-value returns land (L3), carrier methods that would
  return a new value return ring-lifetime `const char *` text (the
  `c_str()` contract) or mutate in place returning the receiver.

See `docs/rules/value-first.md` for the reasoning.
