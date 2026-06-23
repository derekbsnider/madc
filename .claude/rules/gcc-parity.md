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
- GCC is also the PERFORMANCE baseline. When madc's compile/front-end time
  exceeds GCC's on a test, treat it as a pathology signal and callgrind it —
  do not let it slide. Use `scripts/perf_vs_gcc.sh <file>` (auto-callgrinds
  when madc is slower than the threshold); log findings in
  `docs/plans/2026-06-23-parser-lookahead-audit.md`.
- See `.claude/rules/gcc-methodology.md` for the full methodology:
  compare-before-changing, fix-at-deepest-layer, operator
  self-determination, think-first principles.
