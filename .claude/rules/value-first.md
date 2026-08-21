# Value-First madc Code

Applies to ALL madc-dialect code written in this repo: examples,
showcase programs, docs samples, and new tests.

- madc-dialect code carries ZERO `#include`s, ZERO `using`, and ZERO
  `std::` qualification: intrinsics are always available, and the
  auto-include scan + auto-namespace resolution serve bare
  `print`/`println`/`format`, the namespace surfaces
  (`php::`/`ui::`/`madc::` — e.g. `madc::getline` for line input), and
  `cin` — in the main file AND inside quoted user modules. A name that
  needs an include or a `std::` prefix to resolve in dialect code is a
  COMPILER GAP: fix the resolver/scan, never spell around it.
- Prefer `var` / `madc::value` over `std::string`. Use `std::string`
  only when the point of the code IS C++/libstdc++ interop.
- Output goes through bare `print` / `println` / `format`
  (always-included madc intrinsics); a line that ends in `\n` under
  `print` is a `println` line. Never `cout` / iostream output in
  madc-dialect code; error/wall output is `println(stderr, ...)`.
  `printf` only in low-level plumbing tests.
- A string-kind value must be usable like a `std::string`. A missing
  string capability on the carrier is a FEEDER GAP: fix it at the
  carrier (ddARRAY script-method registry in `add_array_methods()` +
  `madarray_*` runtime entries; `php::` parity functions for PHP-shaped
  operations) — never fall back to `std::string` to dodge the gap.
- Until value-by-value returns land (L3), carrier methods that would
  return a new value return ring-lifetime `const char *` text (the
  `c_str()` contract) or mutate in place returning the receiver.

See `docs/rules/value-first.md` for the reasoning.
