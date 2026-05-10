# GCC Parity Rules

- When debugging JIT vs EXE / AOT parity, use GCC as the reference
  compiler before changing codegen or ELF export logic.
- Reduce the failing madc case to the closest valid GCC analogue and
  inspect GCC's emitted code shape first.
- Prefer fixes that move madc toward GCC's lowering shape when that
  shape preserves madc semantics and reduces special-case machinery.
- If madc intentionally diverges from GCC, document the reason in the
  commit or relevant docs before merging.
