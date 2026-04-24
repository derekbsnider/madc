# Typed-Register IR Plan (Draft)

**Status:** Draft — discussion document. Not a commitment.
**Date:** 2026-04-24

## Why now

Since v0.9.1 every fix has followed the same pattern:

> Some `TokenXxx::compile()` got one axis of the operand-shape decision
> wrong — Mem-vs-Reg, narrow-vs-wide, pointer-vs-value, or Gp-vs-Xmm —
> and the mistake only surfaced when a new call site composed tokens in
> a new way.

Recent evidence in just the last two sessions:

- `*e == 0` (Mem operand in `safecmp`) → commit `6318e6b`
- `!=` / `<` / `<=` / `>` / `>=` with Mem destinations → `331b294`
- `v.x += 2.5;` (Mem member load via `load_mem_to_gpq` for real type) → `69f5d06`
- `**pp = v;` (LHS path fell through to a branch that dereferenced NULL) → `9715b71`
- `float a = 1.5f; printf("%f", a);` (float Xmm passed without cvtss2sd promotion) → `e8d96fa`
- `printf("%f", v.x)` on a struct-member double — **still broken**; output
  depends on the JIT source filename length, because the asmjit
  Compiler's register allocator reorders or elides instructions under
  specific register-pressure shapes. The asm looks right on inspection.

Each fix is small. The cluster is not small — and the failure modes are
always "composition of per-token logic mis-coerced an operand." The last
item especially is the signal: the pointwise code looks correct but the
whole doesn't, because there's no shared ground truth about what shape
each value has.

The proposal is to make operand shape an **explicit IR node**, not a
fact that every `compile()` re-derives from surrounding context.

## What this is NOT

- **Not full SSA.** No dominance, no phi nodes, no value renaming, no
  register allocation on our side. asmjit's Compiler still does register
  allocation and emission.
- **Not a new language.** Same source surface, same AST shape, same
  test suite. The only thing that changes is what sits between
  `parseStatement()`'s AST and `cc.movsd(...)` in `compile()`.
- **Not a rewrite.** Token classes stay. `compile()` keeps its name and
  signature. The internals of `compile()` emit IR nodes instead of
  reaching directly into asmjit.
- **Not an optimization pass.** No DCE, no CSE, no constant folding yet.
  The goal is *correctness* of operand-shape handling, not speed. An
  optimization layer can sit on top later if perf demands it.

## Core idea — canonical operand shape

Every value in the IR has a `(type, shape)` label:

| Shape | Meaning |
|-------|---------|
| `Reg`    | Virtual asmjit Gp or Xmm holding the value |
| `Mem`    | Memory location holding the value (stack slot, heap pointer, struct member) |
| `Imm`    | Compile-time constant |
| `Addr`   | A pointer-to-value in a Gp, i.e. `&value` not `value` |

And `type` is a `DataDef *` we already have.

The IR has a small set of nodes:

- **`Load(src: Mem|Addr, type) -> Reg`** — emit the right move for the
  type (mov / movsxd / movsx / movzx / movsd / movss).
- **`Store(dst: Mem|Addr, src: Reg|Imm, type)`** — emit the right store
  (mov / movsd / movss, with cvtsd2ss / cvtss2sd when `src.type` and
  `type` disagree).
- **`Coerce(src: Reg|Mem|Imm, from_type, to_type) -> Reg`** — the one
  place sign-extension / zero-extension / float-double conversion /
  string-to-cstr / pointer-scale lives. Today these decisions are spread
  across TokenAdd, TokenSub, TokenCast, TokenCallFunc, safemov,
  safeadd, ..., and they drift.
- **`BinOp(op, lhs, rhs, result_type) -> Reg`** — normalized. lhs and
  rhs are already the right shape before we get here (the builder
  emitted any needed Load/Coerce). The emitter never sees a Mem+Gp
  arithmetic op; if it did, that's a builder bug.
- **`Cmp(op, lhs, rhs) -> FlagsOrReg`** — same story. Compare shapes
  are normalized first.
- **`Call(fn, args, ret_type) -> Reg|Void`** — args are already
  coerced and in the right shape for the ABI (varargs Xmm is already
  double, pointer args are Gp, etc.). Variadic promotion happens in
  the builder when the arg is assembled, not inside `Call` emission.
- **`Branch / Label / Jump`** — control flow primitives; same
  semantics as asmjit labels/jumps but the IR owns the graph.

That's the shape. No phi, no dominance — just: by the time we reach an
emitter node, the operands have known shapes and types, and any
cross-shape / cross-type conversion was named explicitly upstream.

## How it fixes the recent bugs

- **`*e == 0`:** the comparison builder emits `Load(Mem, ddCHAR) ->
  Reg`, then `Cmp(op, Reg, Imm)`. The `safecmp(Mem, Mem)` failure mode
  can't happen because `Cmp` never sees a Mem.
- **Comparison-to-Mem-destination:** the assignment builder sees
  `Store(Mem, Reg, ddINT64)` — the 0/1 from `Cmp`. Mem-as-setcc-target
  never occurs.
- **`v.x += 2.5;`:** the compound-assign builder emits `Load(Mem,
  ddDOUBLE) -> Xmm`, `BinOp(+, Xmm, Xmm, ddDOUBLE)`, `Store(Mem, Xmm,
  ddDOUBLE)`. No "load real member into Gpq" path exists to pick the
  wrong register class.
- **`**pp = v;`:** the LHS-compile step for an assignment target
  returns an `Addr` (pointer-to-int in a Gp). `Store(Addr, Reg,
  ddINT64)` just works. TokenDeref vs TokenDerefExpr stops being two
  separate branches in TokenAssign — both produce `Addr`.
- **Float varargs promotion:** in the `Call` builder, every arg with
  `type.is_real()` goes through `Coerce(src, from, ddDOUBLE)` before
  it reaches the `Call` node. If `from == ddFLOAT` the coerce emits
  cvtss2sd; if `from == ddDOUBLE` it's a no-op. Single place.
- **Struct-member double `printf`:** harder — this is an asmjit
  register-pressure bug downstream. But the IR gives us control over
  the exact sequence of loads/stores that reach asmjit, so we can
  force a particular shape (e.g. always materialize into a fresh Xmm
  right before the call, no reuse) that the allocator handles
  cleanly. Worst case, we drop out of the asmjit Compiler for this
  specific sequence and emit the instructions directly.

## Migration strategy — bottom up

The 170-test baseline is our safety net. Break it and we stop.

**Stage 0 — scaffolding.** Add the IR node types (`include/madc_ir.h` or
similar). Add an `IRBuilder` helper on `Program`. No `compile()`
changes yet. Unit tests for Load/Store/Coerce emission in isolation.

**Stage 1 — leaves.** Port `TokenInt`, `TokenReal`, `TokenChar`,
`TokenVar`, `TokenAddrOf`, `TokenDeref`, `TokenDerefExpr`,
`TokenMember`, `TokenSubscript`, `TokenSubscriptExpr` to emit IR
instead of directly calling `cc.mov(...)`. These are the sources of
values. Each compile method returns an IR ValueRef. Full test suite
green after each port.

**Stage 2 — arithmetic.** `TokenAdd`, `TokenSub`, `TokenMul`,
`TokenDiv`, `TokenMod`, comparison ops, bitwise ops, compound-assigns.
The `safeadd` / `safecmp` / `safemul` dispatch tree collapses into
`BinOp` / `Cmp` IR nodes; the emitter side of each becomes simpler
because it trusts its operand shapes.

**Stage 3 — calls.** `TokenCallFunc`, `TokenCallMethod`, the dlsym
variadic path, multi-return, struct-member-fn-ptr dispatch. This is
the densest concentration of coercion bugs today and benefits most
from centralizing.

**Stage 4 — control flow.** `TokenIf`, `TokenFor`, `TokenWhile`,
`TokenDo`, `TokenSwitch`, `TokenTerQ`. Labels and branches become IR
nodes. `regdp` reset (`.claude/rules/regdp-reset.md`) becomes
unnecessary because the IR owns the dataflow between sub-expressions.

**Stage 5 — cleanup.** `safemov` / `safeadd` / `safesub` / `safecmp`
in `typesafe.cpp` shrink to just the low-level emitter bodies (no
more cross-shape dispatching; the IR builder already normalized).
`resolveCompoundLHS` and its CompoundLHS struct can likely be deleted
— they exist to paper over the same axis that the IR makes explicit.

At each stage: tests green, commits small, feature-flagged where
useful. If a stage blocks, we can always leave a token un-ported and
let it keep its current code path — the IR and the direct asmjit path
can coexist per-token through the migration.

## What stays the same

- asmjit handles register allocation and x86 emission. The IR is
  input to asmjit, not a replacement.
- The existing `compile(Program&, regdefp_t&)` signature. Tokens still
  emit code; they just emit IR nodes now, which the IR builder later
  emits to asmjit.
- All 170 integration tests, the doctest unit tests, the test-fixture
  convention.
- The `DBG()` macro, the `.claude/rules/` tree, the embedded-header
  system.

## Risks

- **Perf.** Adding an IR layer means one more pass per function
  compile. madc's compile time isn't a bottleneck today (sub-second on
  ~full SMAUG), and the IR is linear — allocate nodes, emit. Unlikely
  to be noticeable; will measure.
- **asmjit Compiler interaction.** The last bug (struct-member double
  printf) shows asmjit's register allocator has its own opinions. The
  IR may need to stay at a level where those opinions don't conflict.
  Mitigation: keep the IR close to what we'd hand-write in asmjit;
  don't try to out-allocate it.
- **Scope creep.** "While we're rewriting, can we also add X?" No.
  The IR ports existing behavior. New features go in after.
- **Half-migration.** A partial migration where some tokens emit IR
  and others don't is fine per-token but painful if the IR builder's
  output shape doesn't compose with the direct-asmjit shape. Needs a
  convention for IR-to-direct and direct-to-IR adapters at the token
  boundary.

## Budget (very rough)

- Stage 0 (scaffolding): 2–3 days.
- Stage 1 (leaves): 1 week.
- Stage 2 (arithmetic): 1 week.
- Stage 3 (calls): 1–2 weeks (densest).
- Stage 4 (control flow): 1 week.
- Stage 5 (cleanup): 2–3 days.

Total: 4–6 weeks of focused work, running in parallel with MadSMAUG
porting (which hits IR-ported paths first and thereby validates them).

## Open questions (for discussion)

1. **IR owner.** Should the IR builder be part of `Program` (lives as
   long as compilation does), or a fresh object per function? Per
   function is cleaner but may duplicate the builder setup cost.
2. **Node lifetime.** Arena-allocate IR nodes and free after asmjit
   emission, or intern them for debugging/optimization? Arena is
   simpler; interning lets a future CSE pass work.
3. **Emit-as-you-build vs build-then-emit.** Today every `compile()`
   emits asmjit instructions immediately. The simplest IR would too —
   `IRBuilder::Load(mem, ddDOUBLE)` calls `cc.movsd(...)` right away
   and returns a `ValueRef`. A "real" IR would defer emission until
   the whole function is built, which enables reordering later but
   complicates Stage 1. Recommendation: start with immediate emission,
   refactor to deferred only if a later stage needs it.
4. **Which token ports first as the proof?** I'd nominate
   `TokenMember` — it's the single token with the most real bugs in
   recent weeks, and porting it exercises the Load/Addr/Coerce shape
   decisions end-to-end. If it's clean on its own, that's a strong
   signal.
5. **Feature-flag strategy.** `#ifdef IR_LEAVES` / `#ifdef IR_ARITH`
   / ... per-stage, removed once the stage is stable? Or one `IR_ALL`
   that flips when migration is complete? The per-stage version
   bloats the code but lets us bisect regressions.
6. **Do we even need Stage 5 cleanup as part of this plan?** The
   `safemov` / `safeadd` / etc. helpers could keep existing (now
   simplified by the IR) and be deleted in a follow-up.

## Recommended first commit

Before any token ports, land Stage 0:

```
src/madc_ir.h         -- IR node and shape definitions
src/madc_ir.cpp       -- IRBuilder with Load / Store / Coerce
                         / BinOp / Cmp / Call / Branch
tests/unit/test_ir.cpp -- doctest cases for each IR node's
                         emission shape (Load of Mem<4>
                         signed → movsxd; Load of Mem<8>
                         real → movsd; Coerce float→double
                         → cvtss2sd; etc.)
docs/rules/typed-register-ir.md -- reasoning for the IR design
.claude/rules/typed-register-ir.md -- bare rules for token ports
                                      (e.g. "tokens emit
                                      IR, not asmjit
                                      directly, after Stage 1")
```

That commit leaves everything else untouched. 170 tests green,
baseline preserved, and the IR is on the shelf ready for Stage 1.
