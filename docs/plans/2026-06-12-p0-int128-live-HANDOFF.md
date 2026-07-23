# HANDOFF — P0 wide-integer track: __int128 LIVE (slices 1+1.5 done); next = slice 2 (value pool / literal pipeline)

Date: 2026-06-12 (succeeds `2026-06-12-embedding-track-complete-HANDOFF.md` for
the ACTIVE track only; that doc remains the develop-state contract). This is
the **current cold-restart contract** for the P0 branch work.

## RESTART STEPS

1. `bash scripts/resume.sh` (STEP 0).
2. Read this file, then `docs/plans/2026-06-12-p0-value-pool-plan.md` (the
   execution plan; its STATUS UPDATE banner is the live chronology).
3. **TRUST THE LIVE REPO**: `git log --oneline -5` on BOTH
   `/workspace/madc` (branch `feature/p0-value-pool-claude`) and
   `/workspace/mir` (branch `feature/scalar-int128-claude`).

## STATE (verified at write time)

- **madc:** `feature/p0-value-pool-claude` (off develop @ `888acb7`), working
  tree clean. Commits: `a651b9a` (slice 1 — gated type plumbing),
  `a6d2727` (slice 1.5 — guard REMOVED, `__int128` real 16-byte end-to-end;
  overflow builtins pass through native; `tests/testint128.mad` == gcc -O0;
  MIR_COMMIT bumped), + a status mirror-sync commit.
- **mir fork:** `feature/scalar-int128-claude` @ `545ad46`
  (`1ee0961` scalar-int128 raise + `545ad46` gcc-correct overflow builtins).
  `MIR_COMMIT` = `545ad46`. **Merge fork→fork-develop AND PUSH when the madc
  branch merges to develop** (pin discipline) — not before, not forgotten.
- **Gates at HEAD (all green):** fulltest **581/0/0/18** exit 0, both check
  gates GREEN, zero warnings; gcc.c-torture **1569/34/19/0/63** — failset
  **55 → 53** (pr122943 + pr63302 FIXED, ZERO regressions;
  `docs/parity/torture-failset-current.txt` updated = the new diff baseline);
  c2mir-test suite green incl. bootstrap; SMAUG soak ready + exit 124.
- **SMAUG soak command changed** (the SMAUG.mad umbrella is GONE):
  `cd /workspace/MadSMAUG/runtime/area && timeout 50 /workspace/madc/bin/madc
  --project /workspace/MadSMAUG/compile_commands.json -lcrypt 4000`
  (good = exit 124 + grep "Realms of Despair ready at").

## WHAT LANDED (summary; details in the plan doc + commit messages)

1. Real `dtINT128`/`dtUINT128` + `ddINT128`/`ddUINT128` (16-byte, align 16);
   type predicates rewritten as EXPLICIT SETS (`is_integer`/`is_real` were
   range-based over the enum and would misclassify tail entries); typeid
   slots 19/20 backed; CIR emits `[N_UNSIGNED,] N_INT128`; `--emit=c11`
   renders `__int128`; PCH spelling map.
2. **c2mir scalar-int128 raise** (the fork): scalar gen dispatch REUSES the
   one-lane-v128 halves emitters via a synthesized one-lane vector type
   (`int128_one_lane_vector_type`); portable 128-bit const folding
   (`int128_pair_*`); SysV two-INTEGER-eightbyte ABI classify; int128↔float
   libgcc-named helpers (`mir-int128-helper.h`); switch (halves compare
   chain), truthiness, inc/dec, implicit conversions all directions
   (`int128_mem_to_scalar` / `scalar_to_int128_mem` are the two funnels);
   `__SIZEOF_INT128__` + `__int128_t`/`__uint128_t` now defined by c2mir.
3. **Overflow builtins went native**: madc's textual remap of
   `__builtin_add/sub/mul_overflow` → 64-bit `__madc_*` helpers REMOVED
   (the `_p` variants stay remapped); registered 0-param in
   `builtin_registry` (args keep compile-time types) + proto-skip via
   `is_c2mir_builtin_call_name`. On the c2mir side fixed: uninitialized
   VALUE-form flag (branch skipped the 0-store into a garbage temp — genuine
   pre-existing bug), narrow (<int) result types rejected by check, and
   mixed-type semantics (native insns now only when both operand types ==
   result type; else a general path computes at 128 bits = exact infinite
   precision for ≤64-bit operands).

## NEXT — slice 2 (then 3), per the plan doc

- **Slice 2 — value pool + literal pipeline:** out-of-line pool (width +
  uint64 limbs, `uint32` handle, ≤64-bit inline fast path — the same
  reference shape P1's flat token record carries: `type_id` + value-handle);
  lexer literal readers (dec/hex/oct/bin, lexer.cpp ~2990-3010 +
  `resolve_int_suffix_type`) accumulate at 128 bits and emit the gcc-parity
  "integer constant is too large for its type" WARNING + truncate (gcc
  warns+truncates, clang errors, madc today is SILENT — verified);
  `TokenInt` kind/handle split (`_token` stays the ≤64-bit fast path).
- **Slice 3 — widen the int64-capped fold rungs:** `ival()`/`ioperate()`
  (tokens.h), `parse_constant_*` (parser.cpp ~5621+), TokenVar const reads
  (datatokens.h). KNOWN RESIDUAL this fixes: **switch case labels >64 bits
  truncate** (madc's parse-time fold; the c2mir switch itself is correct —
  testint128.mad documents it). Also: VLA-sizeof-in-const-context +
  alignof(vla) (pre-existing, status known-gaps).
- `_BitInt` stays FENCED (no c2mir support; Tier-2 raise later).
- Float→int128 IMPLICIT conversions use the signed helper (explicit casts
  pick by target signedness; divergence is UB-only inputs) — noted in code.

## GOTCHAS discovered this session (cold-start traps)

- **Raw `c2m` torture results were misleading before this work**: c2m didn't
  define `__SIZEOF_INT128__`, so `#ifdef __SIZEOF_INT128__` tests silently
  took the long-long branch. It defines it NOW — c2m exercises real int128.
- **madc's CLI does NOT propagate script main()'s return as its exit code**
  — torture passes/fails via abort(), not return codes. Don't "verify" with
  `return N` reducers; use printf/abort.
- In c2mir gen, int128 operands must be materialized at the NODE level
  (`materialize_int128_scalar_node` / `materialize_int128_vector_node`) when
  a >64-bit CONSTANT is possible — `val_gen` of an int128 const yields only
  the low 64 bits. Value-level ops are fine once you hold a 16-byte MEM.
- MIR validates that `B[U]O` is ADJACENT to its overflow insn (only
  stores/reg-moves between) — don't insert MOV-immediates there.
- `mirc_x86_64_linux.h` is real C source text — typedefs are allowed in it.
- The DataType enum is APPEND-ONLY (tail after dtSIMD); the ptr/ref variants
  are +10000/+20000 arithmetic; predicates must stay explicit-set.
- **USER FEEDBACK (standing):** reply to mid-task user messages BEFORE more
  tool calls; pause at scope expansions while the user is active; completion
  claims must state the NOT-covered boundary. Post a short progress note at
  each commit boundary. (memory: feedback_reply_before_working)

## Merge checklist (when slices are done / user says merge)

1. mir: merge `feature/scalar-int128-claude` → fork `develop`, push.
2. madc: confirm `MIR_COMMIT` == pushed fork commit; merge the feature
   branch → develop (--no-ff, user-approved); full gates on develop.
3. Mirror sync (status/CHANGELOG/ROADMAP/KG/memory) at the track milestone.
