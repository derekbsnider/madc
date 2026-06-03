# Clang Parity Rules

- Use clang as the reference compiler for ALL codegen, type-system,
  and runtime behavior questions — not just JIT/EXE parity.
- Run `clang -S -O0` on any failing test BEFORE forming a hypothesis.
  Read the assembly. Understand what clang does. For annotated output,
  also try `gcc -S -fverbose-asm -O0`.
- Reduce the failing madc case to the closest valid clang analogue and
  inspect clang's emitted code shape first.
- Prefer fixes that move madc toward clang's lowering shape when that
  shape preserves madc semantics and reduces special-case machinery.
- If madc intentionally diverges from clang, document the reason in the
  commit or relevant docs before merging.
- See `.claude/rules/clang-methodology.md` for the full methodology:
  compare-before-changing, fix-at-deepest-layer, operator
  self-determination, think-first principles.
