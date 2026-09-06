# C++ conformance lane — a third-party C++ test suite measuring C++11 parity

**Owner directive (2026-09-04):** "we do not currently have a C++ specific
third-party test suite... we should really plan to employ a C++ test suite
that proves we at least have C++11 parity." And, same exchange: **"we don't
want C++11 parity to block any releases, just as a goal on the roadmap."**
So: a MEASURED lane and a ROADMAP goal — never a push or release gate.

## What exists today, and the hole

| Lane | Suite | Standard | Gate |
|---|---|---|---|
| gcc-torture | gcc.c-torture/execute (1652 in scope) | C | promote gate: class-(a) zero (`docs/parity/failset-classification.md`) |
| c-testsuite | c-testsuite (220 single-exec) | C | push-gated ledger lane, ratchet baseline |
| libcxx_gate | 15 hand-written legs | C++ (madc's own shapes) | fulltest gate |
| tests/*.mad | ~1300 madc-authored tests | C, C++, dialect | fulltest |

Every C++ claim madc makes rests on tests madc's own authors wrote. Nothing
third-party asks the C++11 question independently.

## The suite: gcc's g++ testsuite (already on disk at /workspace/gcc)

`gcc/testsuite/g++.dg` and `g++.old-deja` are DejaGnu suites; each runnable
test is marked `{ dg-do run }`, optionally with a standard target
(`{ target c++11 }`, `{ target c++14 }`, …). The C++11 subset is therefore a
FILTER, not a curation. Measured 2026-09-04 (gcc master checkout):

| Subset | Files |
|---|---|
| g++.dg `dg-do run` | 1861 |
| of which no target clause (C++98-era, valid in C++11) | 1106 |
| of which `target c++11` | 349 |
| of which `target c++14` or later (out of scope for the first lane) | 266 |
| g++.old-deja `dg-do run` (C++98) | 877 |

**C++11-parity in-scope run set ≈ 1106 + 349 + 877 = ~2330 tests.** Compile-only
(`dg-do compile`) and diagnostic (`dg-error`) tests are a later phase.

Why this suite first: it is the g++ oracle's own conformance evidence (Rule #1,
gcc is canon), it is versioned with the compiler madc compares against, it is
on disk, and madc already has a runner shaped for it.

Alternatives considered (later phases, not first): clang/test/CXX (mostly
`-verify` diagnostic tests — the diagnostics-parity suite, once the run set is
green); the libc++ test suite (libcxx/test/std — LIBRARY conformance through the
compiler; enormous, and libc++ is macOS's flavor so it belongs with the
darwin-suite lane); llvm-test-suite SingleSource C++ programs (whole-program
oracles; a handful, useful as smoke).

## Design (one runner, not a parallel implementation)

1. **Runner:** extend `scripts/run_gcc_testsuite.py` with a `--suite g++`
   mode (the C torture runner already parses per-test expectations and drives
   madc; a second runner would be the divergence `no-parallel-implementations`
   forbids). Sources: `g++.dg/**` + `g++.old-deja/**` files carrying
   `dg-do run` whose target clause is absent or `c++11`; `dg-options` honoured
   where madc has the flag (`-std=…` mapped to `--std=`); `dg-output`
   compared when present; tests with `dg-additional-sources`, `dg-require-*`,
   `dg-additional-options` madc cannot take, or `-fno-*` GNU switches are
   listed in a skip manifest with a one-line reason each
   (`docs/parity/gxx-skip-manifest.txt`, the twin of
   `torture-skip-manifest.txt`). Standard invoked: `--std=c++11` — the lane
   proves C++11, not "whatever madc defaults to".
2. **Oracle:** the dg expectation itself — a run test passes when it compiles,
   runs, exits 0 and does not abort (`dg-do run` tests `abort()` on failure);
   `dg-output` when given. g++ on the container is the tie-break for any test
   whose own expectation is unclear.
3. **First run = MEASUREMENT** (owner rule: measure before designing). The
   failure list is classified the way `failset-classification.md` classifies
   the C torture set: (a) standard C++11 — must fix; (b) GNU/g++ extensions —
   roadmap; (c) structurally out of domain (inline asm, DejaGnu harness
   features, target-specific ABI probes) — formally skipped with a reason.
4. **Lane:** `scripts/gxx_lane.sh`, a ratchet against
   `docs/parity/gxx-c++11-baseline.txt` exactly like `c_testsuite_lane.sh`
   (RED on a non-baseline failure, LOUD on a baseline test that now passes,
   so the baseline only shrinks) — the ratchet is a REGRESSION SIGNAL for
   whoever runs it, not a gate. Ledger row `gxx-c++11` with `promote=no`
   (recorded for the record, blocks nothing — owner ruling 2026-09-04: C++11
   parity blocks no release). The roadmap goal "C++11 parity" is the
   measured statement **class-(a) C++11 failures = 0**; the count is the
   progress metric on `docs/plans/ROADMAP.md`, not a criterion in
   `branching.md`.
5. **Where it runs:** the container only (QNAP never builds/tests), through
   `remote_build.sh` as a `gxx` stage, under the standard caps (ulimit -t +
   timeout per test — never an un-capped test invocation).

## Sequencing

- After the darwin-host wave in flight (ledger + push + dispatch #8).
- Step 1 (runner mode + skip manifest) and step 3 (first measurement) are one
  slice; the classification and baseline file are the slice's deliverable.
- The lane joins the ledger (`promote=no`) when the baseline exists; the
  class-(a) count joins the ROADMAP as the goal's metric. It never joins the
  push or release gates.

## Thread-safety contract

None new: the lane is a script driving `bin/madc` one test at a time.
