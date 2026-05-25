# GCC Parity Rules

- Use GCC as the reference compiler for ALL codegen, type-system,
  and runtime behavior questions — not just JIT/EXE parity.
- Run `gcc -S -fverbose-asm -O0` on any failing test BEFORE forming a
  hypothesis. Read the assembly. Understand what GCC does.
- Reduce the failing madc case to the closest valid GCC analogue and
  inspect GCC's emitted code shape first.
- Prefer fixes that move madc toward GCC's lowering shape when that
  shape preserves madc semantics and reduces special-case machinery.
- If madc intentionally diverges from GCC, document the reason in the
  commit or relevant docs before merging.
- See `.claude/rules/gcc-methodology.md` for the full methodology:
  compare-before-changing, fix-at-deepest-layer, operator
  self-determination, think-first principles.
