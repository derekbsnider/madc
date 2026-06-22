# Front-End Performance — O(n²) investigation HANDOFF

**Date:** 2026-06-22
**Branch:** `feature/front-end-performance-claude` (NOT develop — develop is clean at the
pushed v0.30.0 `19b275e`). All work below is on the feature branch.
**Status:** P1 (token arena) DONE + committed. Two pre-existing O(n²) front-end
bugs FOUND; one located, one not yet pinpointed. Pick up here post-compaction.

---

## SETTLED — do not re-litigate (verified this session, with evidence)

1. **P1 (TokenStream arena) is DONE, correct, and behavior-neutral.** Two-step
   method: step 1 (`c660eb6`) routed all ~74 token-stream call sites through a
   `TokenStream` seam while it was still a `std::deque` underneath (fulltest
   669/0/0/18 GREEN proved the call-site rewrite neutral); step 2 (`3aaa537`)
   flipped ONLY the method bodies to the flat arena (`_buf` vector + `_cursor` +
   `_pushback` LIFO; backtrack = cursor rewind). fulltest GREEN on the arena too.
   **Do NOT redo or second-guess P1.**

2. **The arena did NOT cause any regression.** gcc-torture failset on the arena
   == failset on the pre-P1 **deque** binary at the same `-O2` (verified by
   building `03e5c75` in a worktree). The 6 deltas vs the 51-name baseline file
   (`memcpy-a{1,2,4,8}`, `memclr` timeouts; `pr84521` SIGSEGV) ALL reproduce on
   the deque binary → **pre-existing at this HEAD, not the arena.** `pr84521`
   also segfaults on pre-P1 (gcc runs it fine) — a separate pre-existing bug.

3. **c2mir / `c2m` is innocent — it is O(n).** `c2m -c` on a 20000-element
   explicit initializer = 0.040s. madc building the same = 0.006s (parse is
   O(n)). The O(n²) is entirely **madc-side, in by-index operand iteration via
   `c2mir_node_op(n,i)`**, which walks the operand DLIST from the head (O(i)).

4. **The arena barely helps these cases** (memcpy-a1 parse 36.4s deque → 33.7s
   arena at -O2) because their cost is NOT the backtrack-copy the arena targets
   (memcpy-a1 has only 2052 re-reads). It is the two O(n²)s below.

5. **`-O2` default (P0) is temporarily reverted to `-O0`** (`310f63e`) for clean
   profiling. RESTORE the `-O2` default (Makefile `CXXFLAGS`) once the O(n²)s are
   fixed — it is a free ~1.74×.

6. **Range designators do NOT explode tokens at parse.** `[0 … 19999]` →
   **32 tokens**, parses in 0.005s. They expand to per-slot designators only at
   EMIT time. The 324K tokens in memcpy-a1 are from **preprocessor macro
   expansion** (`MEMCPY_DEFINE_ONE` × ~648 combos), not range designators.

---

## The two O(n²) bugs (both pre-existing, both madc-side, both via `c2mir_node_op`)

### BUG A — `emit_seq` by-index iteration (EMIT path). LOCATED.
- **Where:** `src/cir_emit_c.cpp:100` `emit_seq()` — `for (i=from;;i++){ c=op(n,i); … }`
  where `op(n,i)=c2mir_node_op(n,i)` is O(i). Same by-index pattern at
  `cir_emit_c.cpp:117, 178, 242, 305`.
- **Symptom:** `--emit=c11` of a 20000-element explicit init = **2.5s**; parse of
  the same is 0.006s. callgrind: **95.87% in `c2mir_node_op`**.
- **Impact:** only `--emit=c11`/`--emit=mc11` (the emit-C fidelity gate + transpile
  output). NOT the JIT run.
- **FIX (pure madc, NO fork change):** `c2mir_node.h` exposes `NL_HEAD`/`NL_NEXT`
  on `n->u.ops` (madc already sees `node_t` internals — cir_builder reads
  `n->code`). Rewrite `emit_seq` (and the 4 sibling loops) to walk operands
  **sequentially** (`NL_HEAD` then `NL_NEXT`), replicating `c2mir_node_op`'s
  leaf-guard (`code <= N_ID || N_CF/N_CD/N_CLD` → no ops). O(n²)→O(n).
  - Alternatively add `c2mir_node_first_op`/`c2mir_node_next_op` to the fork API
    (consistent with the existing `c2mir_op_append`/`op_tail`/`op_splice_after`),
    but the pure-madc `NL_HEAD/NL_NEXT` rewrite avoids the fork pin/push dance —
    PREFER pure-madc.
- **Gate:** `--emit=c11` output **byte-identical** before/after (representation of
  iteration changed, emitted C must not) + re-time big_init (2.5s → ~ms).

### BUG B — parse/build O(n²) on macro-heavy bodies (JIT path). NOT YET PINPOINTED.
- **Symptom:** `memcpy-a1.c` JIT run (NO --emit) `--show-stats`: **parse 28s**,
  c2mir 0.35s, execute 0. 324K tokens at **11,639 tok/s** vs big_init 40K tokens
  at **6.7M tok/s** (~575× slower/token). **This is what times out the torture
  run** (runner's flat 5s cap; it ignores the test's `dg-timeout-factor 8`).
- **Ruled OUT as the trigger (all parse FAST):** range designators (rng_20000 =
  0.005s), large explicit init (big_init = 0.006s), many trivial functions
  (1600 funcs = 0.017s, LINEAR). So it is NOT function-count, NOT init size, NOT
  ranges.
- **Therefore the trigger is something in memcpy-a1's per-function BODY shape**:
  `static a_t dst = {{ [0…87]=0xaa }}` (function-local static aggregate) + the
  `for` verify loops + `__builtin_memcpy` + `asm("":::"memory")`, repeated ×648.
- **NEXT STEP (do this first post-compaction):** build a *representative* reducer
  that mirrors memcpy-a1's function body (local static array init + a loop),
  emit it K times, scale K, confirm O(K²). Then callgrind that reducer (small K,
  a few seconds) and read the top self-cost function. Likely a per-declaration
  or per-statement operation that scans something O(n) (symbol/scope table walk,
  a global list append/scan, or a by-index `c2mir_node_op` in the build path —
  see `cir_builder.cpp:10776, 13236, 13269`). Fix at the deepest layer.
- **Gate:** fulltest GREEN + torture failset == deque-baseline + re-time
  memcpy-a1 parse (28s → seconds).

---

## Reducers (in `tmp/`, gitignored) — reuse them
- `tmp/rng_20000.c` — single `[0…19999]` range init. Parse 0.005s (32 tok); emit 4.2s.
- `tmp/big_init.c` — 20000 explicit ints. Parse 0.006s; emit 2.5s (BUG A).
- `tmp/funcs_{400,800,1600}.c` — N trivial functions. LINEAR (rules out fn-count).
- **TODO reducer:** memcpy-a1-shaped body × K (for BUG B).
- Real test: `gcc_testsuite/gcc.c-torture/execute/memcpy-a1.c` (+ `memcpy-ax.h`).

## Worktrees to clean up (git worktree remove when done)
- `…/scratchpad/preP1` (03e5c75, deque -O2 baseline)
- `…/scratchpad/dev_base` (19b275e, released v0.30.0 -O0; memcpy-a1 parse = 60s)

## Sequencing
1. (post-compaction) BUG B: reducer → callgrind → locate → fix. The torture win.
2. BUG A: emit_seq sequential iteration. The `--emit=c11` win.
3. Re-validate both (fulltest + torture failset vs deque-baseline + emit-c11
   byte-identical), re-time.
4. Restore the `-O2` Makefile default.
5. Merge `feature/front-end-performance-claude` → `develop` via PR when validated
   and CIR-parity is intact.

## Commits on the feature branch (origin/develop..HEAD)
- `310f63e` build: temporarily -O0 (debug)
- `3aaa537` perf(parser): TokenStream flat arena + cursor (P1 step 2)
- `c660eb6` refactor(parser): TokenStream seam (P1 step 1)
- `03e5c75` docs(status): P0 -O2 landed
- `8afae8b` perf(build): -O2 default (P0)
- `07b3266`, `fc4ffec` docs (status + the front-end-performance plan)
