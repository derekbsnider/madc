# C++-First API, C Shim Last

- Design new embedding and `libmadc` interfaces as C++ APIs first.
- Make the C++ layer the single real implementation.
- Keep `extern "C"` shims thin wrappers over the C++ layer.
- Do not design the C++ API around premature C-ABI constraints.
- Make CLI and native-host call sites adopt the C++ API before adding C shims.
- Keep ownership, lifetime, diagnostics, and IO policy modeled in C++ objects.
- Add or expand C shims only after the C++ model is coherent and exercised.
- `extern "C"` exports exist EXCLUSIVELY as the C-linkage API for C hosts
  consuming libmadc.a/.so — never as a script-side resolution path.
- Script-facing embedded headers (`<ns_madc>`, `<ns_php>`, …) declare their
  namespace publics as declaration-only C++ functions, resolved mangled-direct
  (Itanium symbols) to real `namespace X { }` implementations in the host.
- Do not write namespace wrapper bodies over the extern-C exports in script
  headers; that flattens C++ type information (references, overloads).
- Exception: compiler-machinery symbols emitted by the CIR builder
  (`__madc_scope_set_*`, `__madc_vla_free` category) stay extern-C — they are
  not user-resolved functions.
- Exception: a COMPILER-IMPLEMENTED namespace public (`php::print_r`) is
  declared in its script header as a function TEMPLATE with no definition
  anywhere, tagged `FuncDef::inline_builtin_kind`, and lowered by the CIR
  builder. It binds to nothing and exports to nothing — by design.
- See `docs/rules/cpp-first-api.md` for the reasoning.
