# Dialect-Lean — the madc surface never depends on C++ headers or std::string

- OWNER LAW (2026-08-21): under `--std=madc`, the language surface — the
  auto-include prelude fragments included — must NOT depend on parsing
  C++ system headers (`<string>`, `<iostream>`, ...) unless the script
  itself opts into C++ interop by using those types/names.
- OWNER LAW (2026-08-21): nothing the madc language defines within its
  own dialect or the polyglot functionality (`php::`, `python::`, ...)
  depends on std::string — internal madc/engine C++ source is the only
  place it lives.
- Until the frozen-forest/PCH machinery makes C++ header binding
  near-free (measured, not asserted), new dialect capability leans on
  madc builtins and the polyglot namespaces — never on std:: machinery
  in the always-served prelude.
- Embedded dialect fragments (`include/madc/ns_*` extensionless,
  `include/madc/bits/*`) carry ZERO includes. std::string-typed interop
  conveniences are declared only inside
  `#if defined(_GLIBCXX_STRING) || defined(_LIBCPP_STRING)`
  (the stdlib's own guards — the `<ns_madc>` convention).
- One publics list, two renderings: gate lines by the guard within ONE
  fragment file; never a second hand-maintained lean copy.
- Every polyglot public needs a lean PRIMARY form (`value` / `array` /
  `const char*`); a function that exists only in std::string shape is a
  gap, not a contract.
- Ring-lifetime `const char *` (ns_common::ring_slot, the c_str()
  contract) is the pre-L3 return convention for dialect text returns;
  `value` returns arrive with L3.
- Carrier semantics never vary with which headers a TU parsed (the
  subscript SLOT model is the precedent).
- Gate: `scripts/check-dialect-lean.sh` (in fulltest) fails the build on
  a C++ system include or an unguarded std::string in any dialect
  fragment; it carries its own negative control.

See `docs/rules/dialect-lean.md` for the reasoning.
