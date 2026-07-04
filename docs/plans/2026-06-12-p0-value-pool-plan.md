# P0 — Value pool + wide-integer correctness (implementation plan)

**Status:** ✅ COMPLETE (2026-07-04). All slices landed: slice 1+1.5 @a651b9a
(+ mir fork raise), slice 2 @7d7c0e5d (value_pool + 128-bit literal pipeline),
slice 3 @956e7030 (128-bit fold spine + wide case labels; the dead
ioperate/foperate web was DELETED @59653106, not widened — it had no callers).
Landing detail lives in
`docs/plans/2026-07-04-data-substrate-first-customer-PLAN.md` §Landing history.

**Historical status:** ACTIVE (2026-06-12). Branch `feature/p0-value-pool-claude`.

> **STATUS UPDATE (2026-06-12, later):** slice 1 committed gated (`a651b9a`);
> **slice 1.5 (the c2mir scalar-int128 raise) IMPLEMENTED** on the mir fork
> branch `feature/scalar-int128-claude` @ `1ee0961` (c2mir-test suite green
> incl. bootstrap) and the madc guard FLIPPED (guard removed; MIR_COMMIT
> bumped in the same commit). c2mir now compiles scalar __int128 end-to-end:
> gen dispatch onto the one-lane-vector halves emitters, 128-bit const
> folding, SysV two-eightbyte ABI, int128<->float helper conversions,
> overflow builtins, switch, truthiness, inc/dec, implicit conversions.
> `__SIZEOF_INT128__` is now defined by c2mir (x86_64 linux) +
> `__int128_t`/`__uint128_t` builtin typedefs.
> madc additionally stopped remapping `__builtin_add/sub/mul_overflow` to the
> 64-bit `__madc_*` helpers — they pass through as real c2mir builtins
> (registered 0-param so args keep their compile-time types); the `*_p`
> variants stay remapped. `tests/testint128.mad` output == gcc -O0.
> Torture int128 set via madc: 13/14 (+2 former failset entries pr122943 +
> pr63302 now pass; pr92904 remains failset for its unrelated aligned-attr
> gap). **Slice-3 residual found:** case labels >64 bits truncate in madc's
> parse-time constant fold (`parse_constant_*` rungs) — the int128 switch
> itself works; only the >64-bit LABEL value is wrong. Float→int128 implicit
> (non-cast) conversions use the signed helper (explicit casts pick by
> target signedness; divergence is UB-only inputs).
Executes **P0** of `2026-06-09-frontend-representation-refactor.md` — rung 1 of
the forest-prerequisite ladder in `2026-06-12-embedding-track-complete-HANDOFF.md`.
The TYPE-side substrate (typeid table + 32-byte `madc_value` ABI) already landed
via the eval track; this is the TOKEN/value side.

**Rung 0a verified complete at live HEAD (2026-06-12):** `cout << std::string`,
`testfstream`/`testloop`/`testdefer` all green with `--std=c++17
--no-embedded-headers`; `try_std_free_function_call` retired (system-header free
fns bind mangled-direct, data-driven; polyglot-ns `__ns_` wrappers are by-design).
The status `in_progress` walls are stale — mirror-sync at session end.

## Verified facts the plan rests on

- **c2mir fork `__int128` is PARTIAL** (corrected 2026-06-12 by direct probe):
  the type system (`N_INT128` → `TP_INT128`/`TP_UINT128`, 16-byte size/align),
  128-bit CONSTANT folding (`set_int128_const_bits`/`c_u_hi_val`), and one-lane
  v128 int128 VECTOR ops all work — but **scalar `__int128` variables have no
  MIR lowering** (`get_mir_type` returns `MIR_T_UNDEF`, c2mir.c:12081; stock
  `c2m -ei/-eg` fatals on `__int128 a = 1; return (int)a;` with "wrong type
  memory" / "undeclared func reg"). Slice 1's type flip therefore REGRESSES the
  11 torture int128 tests that today pass through madc's 64-bit fake (measured:
  10 fatal, only vector-based pr105613 survives).
- **Consequence — slice ordering changed:** slice 1 lands as INERT plumbing
  with the lexer/PCH flip behind `#ifdef MADC_INT128_REAL` (default OFF; zero
  behavior change), and a new **slice 1.5 — c2mir scalar-int128 raise** (Tier-2
  per lowering-vs-raising, the `_BitInt` family precedent) does the fork work:
  scalar TP_INT128 load/store/arith/compare/convert lowering reusing the
  one-lane-v128 int128 lane machinery + existing div/mod helper calls. The
  madc flip (guard default ON) + `tests/testint128.mad` + the `MIR_COMMIT` bump
  land in the SAME madc commit (pin discipline). madc also currently defines
  `__SIZEOF_INT128__=16` (predefined_macros.cpp:364) while int128 is a 64-bit
  fake — the lie that makes the fix mandatory rather than optional.
- **c2mir has NO `_BitInt`** — `_BitInt` codegen is a Tier-2 raise, FENCED out of
  P0; only the pool representation accommodates arbitrary width.
- **Canon on >64-bit literals:** gcc = warning "integer constant is too large for
  its type" + truncate; clang = hard error. madc today silently truncates (the
  bug). madc follows gcc (warn + truncate keeps code compiling).
- **DataType predicates are range-based** (`is_integer` = `< dtFLOAT`,
  `is_real` = `[dtFLOAT, dtRESERVED)`), and all enum slots below `dtFLOAT=12`
  are taken → new values append at the tail (16/17; append-only enum
  discipline) and the predicates become explicit sets. Range logic is confined
  to datadef.h:149-167 (grep-verified).
- **PCH maps builtin types by SPELLING** (`builtin_datadef_from_spelling`,
  pch.cpp:248) — no enum-value serialization concern; just remap the
  `__int128` spellings.
- **typeid slots 19/20 are reserved** for exactly this (madc_typeid.h); pin test
  test_datadef.cpp:1550 expects NULL → flips to the real DataDefs (slot numbers
  unchanged).

## Slices (each a commit, each gated)

### Slice 1 — real `__int128` / `unsigned __int128` type, end-to-end
1. `DataType`: append `dtINT128 = 16, dtUINT128` (+ `dtINT128ptr = 10016`,
   `dtINT128ref = 20016` families). `rtPtr`/`rtRef` are +10000/+20000 arithmetic
   — tail values compose.
2. Predicates: `is_integer()` admits the two new values; `is_real()` becomes the
   explicit set {dtFLOAT, dtDOUBLE, dtLDOUBLE}; `is_unsigned()` adds dtUINT128.
3. `DataDefINT128`("__int128", 16) / `DataDefUINT128`("unsigned __int128", 16);
   globals in parser.cpp:6104 block; `alignment()` already yields 16 for size≥16.
4. `madc_primitive_for_slot`: slots 19/20 → the new globals (slot 18 long-double
   stays reserved).
5. Lexer: `TS_INT128` arms (lexer.cpp:3957/3960) return the real DataDefs
   (complex arm unchanged — gcc rejects `_Complex __int128` anyway).
6. PCH: spelling remap for `__int128`/`signed __int128`/`unsigned __int128`.
7. CirBuilder: `native_scalar_specs`, `append_type_specs`, the second mapping
   (~cir_builder.cpp:3283) emit `{N_INT128}` / `{N_UNSIGNED, N_INT128}`;
   `integer_typed` stays N_L/N_UL for ≤64-bit values (c2mir converts).
8. `--emit=c11`: spec switch (cir_emit_c.cpp:584) renders `N_INT128` →
   `__int128` (gcc/clang both accept; portability caveat documented in
   c11-transpiler reasoning).
9. Tests: `tests/testint128.mad` (decl, arithmetic, shifts/compose, sizeof,
   mixed conversions) — output g++-verified; unit pin updates.

### Slice 2 — value pool + literal pipeline diagnostics
- New value pool (width + uint64 limbs, `uint32` handle, ≤64-bit inline
  fast-path) — the handle is the same reference shape P1's flat token record
  carries (`type_id` + value-handle).
- Lexer literal readers (dec/hex/oct/bin) accumulate at 128-bit, store wide
  values in the pool; emit the gcc-parity "integer constant is too large for
  its type" warning + truncate where gcc does.
- `TokenInt`: kind/handle split — `_token` stays the ≤64-bit fast path; wide
  literals carry the pool handle; `ival()` documented as the truncating view.

### Slice 3 — widen the int64-capped fold rungs
- `madc_wide_int` (= host `__int128`) for the constant-fold spine:
  `ioperate()`/`ival()` overrides (tokens.h), `parse_constant_*` rungs
  (parser.cpp), `TokenVar` const reads (datatokens.h data-read chain gains the
  16-byte arm), `evaluate_type_query` sizeof folding.
- `CirBuilder::integer_typed` gains the >64-bit emission: compose
  `((unsigned __int128)hi << 64) | lo` as cir_nodes (Tier-1 lowering — keeps
  `--emit=c11` portable; c2mir folds it back to a 128-bit const at check time).

### Out of scope (fenced)
- `_BitInt(N)` codegen (no c2mir support — Tier-2 raise, roadmapped separately).
- `long double` widening (slot 18), `_Complex` integer types.
- P1 flat-token-buffer itself (this plan only fixes the reference shape it needs).

## Gates (every slice)
`make -C src fulltest` green; gcc.c-torture run ALONE with failset diff vs
`tmp/failset_lsq.txt` (zero regressions); the new wide-integer tests verified
against g++ (canon); SMAUG soak before merge (C89 — int128-unreachable, but the
predicate rewrite touches shared machinery).
