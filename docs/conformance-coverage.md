# Conformance coverage — measured against third-party suites

This is the **one home** for madc's measured conformance numbers. The
user-facing feature catalogue is
[`docs/language/cpp-features.md`](language/cpp-features.md); the compliance
roadmap is [`docs/plans/cpp-support.md`](plans/cpp-support.md); madc's own
integration suite lives in [`docs/test-status.md`](test-status.md). None of
those repeat the figures below — they link here.

Every number is produced by `scripts/run_gcc_testsuite.py` against corpora
madc does not own (gcc's own testsuite, and the third-party
[c-testsuite](https://github.com/c-testsuite/c-testsuite)), so they are not
self-graded.

**Measured on `7488a39cf`** — the content of `develop` at `a6e8bd55f`, whose
release tier is green on every lane (`bin/madc`, the -O0 development build).

These figures were first taken mid-session on `95a5dd936` and then RE-TAKEN
on the shipped content after the last parser fix landed, because that fix
touched C++ member lookup and a coverage table measured on a compiler that no
longer exists is worse than no table. Every figure was identical across both
runs (2026-09-21 01:11–01:16 UTC).

## C

| corpus | `--std=` | in scope | passing | coverage |
|--------|----------|---------:|--------:|---------:|
| `gcc.c-torture/execute` | c17 | 1624 | 1611 | **99.2%** |
| `gcc.c-torture/execute` | c23 | 1624 | 1485 | **91.4%** |
| c-testsuite `single-exec` | gnu11 | 220 | 220 | **100%** |

Both torture columns run the same 1624 tests; 61 further tests are skipped by
`docs/parity/torture-skip-manifest.txt`.

**Read the c17 column as the like-for-like figure, not c23.** The 126 tests
that pass under c17 and fail under c23 are almost entirely old-style (K&R)
function definitions — `f(fmt) char *fmt; { ... }` — which C23 *removed* from
the language. `gcc -std=c23` rejects those same files (verified on
`20000112-1.c`, `20000314-3.c`, `20000726-1.c`: 1 error each under c23, 0
under c17), so madc refusing them is **correct behaviour**, not a gap. The
torture corpus is C-era code; measuring it under c23 measures how much of it
is still valid C23.

The 13 remaining c17 failures are listed in
[`docs/parity/c-torture-baseline.txt`](parity/c-torture-baseline.txt). Every
one was verified to fail on the v0.99.2 release binary as well, so the
baseline holds pre-existing gaps only and no regression is parked there.

## C++

| corpus | `--std=` | in scope | passing | coverage |
|--------|----------|---------:|--------:|---------:|
| `g++.dg/template` | c++98 | 385 | 308 | **80.0%** |
| `g++.dg/cpp0x` | c++11 | 1565 | 1151 | **73.5%** |
| `g++.dg/cpp0x` + `g++.dg/template` | c++11 | 1950 | 1464 | **75.1%** |
| `g++.dg/cpp1y` | c++14 | 417 | 206 | **49.4%** |
| `g++.dg/cpp1z` | c++17 | 308 | 128 | **41.6%** |
| `g++.dg/cpp2a` | c++20 | 653 | 346 | **53.0%** |

The combined `cpp0x + template` row is the **ratchet lane** recorded in
`docs/lane-status.tsv` as `gxx-c++11` — the metric the roadmap tracks
(ROADMAP 2.12). The other rows measure each standard against its own
directory so the eras are comparable.

### What "in scope" means, and why it matters

These figures cover the **compile-clean** subset: tests carrying
`dg-do compile` and *not* carrying `dg-error` / `dg-warning` / `dg-bogus` /
`dg-message`. A diagnostic test asserts that the compiler produces a
*particular* error — madc must emit that diagnostic, not merely fail — which
is a later phase of the conformance work. Those tests, along with
multi-TU `dg-additional-sources` companions and non-compile tests, are
reported as *skipped* and excluded from the denominator: between 284 and 2718
per directory.

So "80.0% for C++98" means 80.0% of the C++98-era tests that must compile
cleanly, not 80.0% of everything gcc ships for C++98. Quoting these numbers
without that qualifier overstates them.

### Cross-era comparison is weaker than it looks

The per-era directories are not equal-difficulty samples of their standards —
each one collects the tests gcc happened to write for that era's features. The
C++17 figure sitting *below* C++20 is real and reproducible, but it reflects
what is in `cpp1z` versus `cpp2a`, not a claim that madc supports C++20 better
than C++17. **A single era's movement over time is the meaningful signal;**
the ranking between eras is not.

### Where to push next

`g++.dg/template` is shared by every standard — it is core-language material,
not C++98-specific — so its 77 remaining failures drag on the C++11, C++14,
C++17 and C++20 measurements simultaneously. Raising C++98 lifts several rows
at once, which is why it is the stated entry point for the next conformance
push (owner, 2026-09-20).

## How to reproduce

```bash
# the fast tier — under two minutes for all three, run it after every commit
bash scripts/fast_lanes.sh

# one corpus at a time
bash scripts/c_torture_lane.sh                     # C, ratcheted
bash scripts/c_testsuite_lane.sh                   # C, ratcheted
bash scripts/gxx_lane.sh                           # C++11, ratcheted

# any other standard: point the runner at that era's directory
python3 scripts/run_gcc_testsuite.py --suite gxx \
        --gxx-dirs g++.dg/cpp1z --std c++17
```

`--root` defaults to the repo's `gcc_testsuite` symlink, which **dangles on
the container** — pass `--root /workspace/gcc_testsuite` there.

## Lane tiering

Suites are tiered by time to run (owner directive, 2026-09-20). The fast tier
runs after every commit; the long lanes gate merges and releases. See
`scripts/lane_ledger.sh` and `docs/lane-status.tsv`.

| tier | suites | cost |
|------|--------|------|
| `commit` | c-testsuite (4s), c-torture (27s) — plus gxx-c++11 (84s), measured but never gating | ~2 min total |
| `yes` | the linux battery, wine, the macOS cross build, the Xvfb `gui` stage | ~1 h |
| `release` | the libc++ flavor lane, the darwin full suite on both arches, genuine Windows | ~2 h |

gcc c-torture is in the fast tier for a reason worth recording: it previously
had no lane script and no ledger row, so nothing re-ran it between 2026-08-12
and 2026-09-20. It drifted from 1614 passing to 1587 while eight other lanes
stayed green, carrying three standard-C regressions — one a parser SIGSEGV —
for five weeks. The run takes under half a minute. It was never expensive; it
was unowned.

The `gui` stage was found in the same state while writing this document — it
existed in `remote_build.sh` but had no ledger row, so nothing required it to
run. It now has one. When a suite exists but nothing gates it, the question
is not whether it passes today but how long it has been since anyone knew.
