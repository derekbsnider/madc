# Lowering vs Raising — where a missing feature gets fixed

When madc needs a construct c2mir/MIR doesn't handle, decide WHERE to fix it.
The deciding test: **can it be expressed faithfully (semantics, ABI,
diagnostics) in a C11 form c2mir already compiles?** Three tiers:

- **Tier 1 — lower / resolve in madc (DEFAULT).** Syntactic or sema-resolvable
  features: resolve at parse/sema and emit ordinary C11 `node_t`. Examples:
  `nullptr`→`(void*)0`, `constexpr`→folded constant, `auto`→resolved type,
  `typeof`→concrete type, `_Generic`→selected expr (note: c2mir ALSO handles
  `_Generic` natively), binary/`'`-sep literals→value, `static_assert`→checked
  then dropped. Also C++→C11 (Cfront): see `.claude/rules/c11-transpiler.md`.
  Preferred because it keeps the fork small AND keeps `--emit=c11` portable to
  any C11 compiler (one lowering serves JIT + interp + transpile).

- **Tier 2 — raise c2mir (fork).** Genuine semantic/type primitives with no
  faithful C11 form: `_BitInt(N)`, `_Decimal*`, atomics/memory-ordering.
  Precedent: native `_Complex` (lowerable to a struct, but raised for fidelity
  + ABI cleanliness — so the bar is "no faithful form, OR lowering is genuinely
  lossy/unmaintainable AND the feature is core enough to justify fork cost").

- **Tier 3 — raise MIR (the floor), + c2mir.** When the primitive is missing
  from MIR itself (no IR representation). Today this is **SIMD/vectors** (MIR
  locals are `i64/f/d/ld` only). Raising MIR is the biggest fork step — do it
  only when decided and roadmapped, and **design it for upstream** (see Track
  1.6 / KG `Decision{simd_raise_mir_upstream}`). Until it lands, the interim is
  **scalarize in madc** (Tier-1 functional correctness) + **emit-C → gcc/clang**
  for real performance (AOT).

Rules of thumb:
- Prefer the highest tier (1) that is *faithful*. Don't raise for pure syntax —
  madc owns the front end (it builds `node_t`; c2mir never sees source).
- A raise that emits non-C11 (e.g. native C23) must STILL keep a Tier-1 lowering
  for the `--emit=c11` path, or it breaks portable-C output. So raising rarely
  *saves* the lowering — weigh accordingly.
- Every Tier-2/3 raise grows fork divergence — minimize it; design Tier-3 for
  upstream so the cost can leave our tree.

See `docs/adr/0001-cir-c2mir-backend.md` for the full reasoning.
