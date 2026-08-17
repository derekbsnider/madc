# Backend Strategy (forward trajectory)

The backend decision is settled — see `docs/adr/0001-cir-c2mir-backend.md`.
Do not re-litigate it; follow the trajectory below.

- The **primary backend** is `madc parser → cir_node (MC11-IR, == c2mir
  node_t) → c2mir → MIR → JIT`. It is the **sole** backend. Do not revive
  asmjit or a parallel codegen.
- madc is a **C/C++ dialect** — C++ lowers to C11 (Cfront); borrowed-language
  conventions (`php::`/`perl::`/… namespaces) are C/C++ libraries; the backend
  (MIR + c2mir) is itself C. Keep it that way: new language features get a
  **C11 lowering**, not a bespoke non-C path.
- **Direct-MIR-via-API is a scalpel, not the backend.** Use it ONLY for runtime
  internals (dispatch trampolines, exception runtime, multi-return shims) and
  the interpreter / REPL / debug tier — emitted into the same MIR module.
  User-visible language features stay on the c2mir path so they also flow
  through `--emit=c11`.
- **`--emit=c11` is a first-class output**, not just the fidelity gate: the
  C-AST IR emits portable C11 for any C toolchain (JIT + interp + transpile are
  three outputs of one IR). Keep emitted C portable (see
  `.claude/rules/c11-transpiler.md` hygiene).
- MIR lives IN this repository at `third_party/mir` (subtree — ordinary
  madc source; no pin, no fork lockstep). `vnmakarov/mir` is upstream;
  `derekbsnider/mir` is historical + upstream-PR transport only (see
  `.claude/rules/build.md`).
- Near-term goal is **CIR coverage parity with master** — it gates promotion to
  master (see `.claude/rules/branching.md`). Drive the integration worklist
  down; don't start downstream tracks (data substrate, ARM64, AOT) before it.

See `docs/adr/0001-cir-c2mir-backend.md` for the full reasoning, alternatives,
and accepted trade-offs.
