# No Parallel Implementations / Anti-Drift — reasoning

## The incident (2026-06-01)

madc had two tree builders feeding c2mir:

- **`CirBuilder::translate_module`** (`src/cir_builder.cpp`) — the live backend,
  what `bin/madc` and every integration test and SMAUG run through.
- **`cir_translate`** (`src/madc_cir.cpp`, ~1330 lines) — a legacy static
  translator, reachable in production only via `MADC_CIR_OLD=1`, "retained for
  A/B comparison during the migration."

The two **drifted**. The legacy path mislowered a backward `goto` loop into an
unconditional infinite loop (it dropped the loop body), while `CirBuilder`
lowered it correctly. Nobody noticed, because nothing in production used the
legacy path — *except* `test_cir`'s `cir_run`, which built its tree with
`cir_translate`. So the unit tests were validating **dead code that had diverged
from the real backend**:

- They gave **false signal** — the unit suite "passed" against a translator that
  isn't the backend, and couldn't catch real `CirBuilder` bugs.
- The divergence (the infinite loop) **hung the unit harness** under MIR_interp.
  With no timeout on the `make test` loop, the hung `test_cir` spun forever and
  pegged the QNAP host. Orphaned background tasks re-launching `make fulltest`
  multiplied the hung processes.

Cost: most of a session spent chasing a "CirBuilder label bug" that was actually
a dead-code drift in a translator we no longer use, plus a host-stability scare.

## Why the rules follow

1. **One implementation.** Two implementations of the same thing is a standing
   drift hazard: the unused one is never stressed by real workloads, so it rots,
   and anything that *does* touch it (a test, a debug flag, a future caller)
   inherits the rot. The fix was to delete `cir_translate` entirely
   (madc_cir.cpp: ~1800 → 275 lines).

2. **A/B scaffolding expires.** Comparing an old and new path during a migration
   is legitimate — but only while *both* are actually run (ideally in CI). The
   moment the new path is the sole production path, the old one is dead weight.
   Gate it behind a deletion deadline so it can't quietly become a trap.

3. **Tests use the production entry points.** A unit test that constructs its
   subject through a different path than production is testing a fiction. Call
   the same functions `bin/madc` calls (`CirBuilder::translate_module`, here).

4. **Cap every test invocation.** A correctness bug can manifest as a non-
   terminating loop. Wall-clock `timeout` catches most hangs; `ulimit -t` (a
   per-process CPU cap, inherited by children) catches the rest even if the
   process ignores SIGTERM. `make test` and `scripts/run_tests.sh` both apply
   these — a hung test now fails the suite instead of taking down the machine.

5. **Let the compiler verify the cut.** When removing a dead web of functions,
   build with `-Wall`: `-Wunused-function` on the remaining members tells you
   the deletion is complete and that nothing live still referenced them. A clean
   build with no such warnings is the proof.
