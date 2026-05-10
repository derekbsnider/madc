# C++-First API, C Shim Last

- Design new embedding and `libmadc` interfaces as C++ APIs first.
- Make the C++ layer the single real implementation.
- Keep `extern "C"` shims thin wrappers over the C++ layer.
- Do not design the C++ API around premature C-ABI constraints.
- Make CLI and native-host call sites adopt the C++ API before adding C shims.
- Keep ownership, lifetime, diagnostics, and IO policy modeled in C++ objects.
- Add or expand C shims only after the C++ model is coherent and exercised.
- See `docs/rules/cpp-first-api.md` for the reasoning.
