# Benchmarks — execution time and compile time vs gcc/g++

Two numbers matter and they move **independently**:

- **Execution time** — the quality of the code madc's backend (MIR) generates.
- **Compile time** — the speed of madc's front end (lexer → parser → cir_node
  → c2mir).

A change can improve one and regress the other, so both are tracked here, side
by side, and **re-measured at every release**. This file is the one home for
those numbers; `docs/conformance-coverage.md` is the one home for correctness
coverage. Neither repeats the other.

Regenerate everything with:

```bash
bash scripts/benchmark_lane.sh                 # prints both tables
MADC_BENCH_TSV=docs/benchmarks/history.tsv \
  bash scripts/benchmark_lane.sh               # also appends a trend row
```

`history.tsv` is the across-releases record: one row per benchmark per run,
stamped with the commit. **The ratio is the number to read, not the
milliseconds** — a ratio survives a change of host, absolute times do not.

## Method

- **Best-of-N wall clock, interleaved.** A sequential A-then-B run penalises
  whichever binary goes second on a cold cache, so the two alternate.
- **Output is compared before any timing is kept.** A wrong answer makes a
  fast time meaningless; a mismatch is reported as `MISMATCH`, never timed.
- **Geometric mean**, because these are ratios.
- Execution compares **native binaries at matched optimization**
  (`gcc -O2` vs `madc -O2 -o`). Compile compares **TU → object**
  (`gcc -O0 -c` vs `madc -c`), because gcc `-O0` is the must-beat front-end bar.
- Wall time is noisy. One run is a trend point, not a verdict.

## Execution — C

MIR's own shootout suite (`third_party/mir/c-benchmarks`), `gcc -O2` vs
`madc -O2`, native binaries both sides. Measured 2026-10-23 on `787813947`
(the released madc 0.106.0), 20-core x86-64 container, gcc 13.3.0.

| benchmark | gcc (ms) | madc (ms) | madc/gcc |
|---|---:|---:|---:|
| hash | 3968 | 3760 | **0.95×** |
| except | 3563 | 3458 | **0.97×** |
| funnkuch-reduce | 1960 | 1950 | **0.99×** |
| heapsort | 5302 | 5273 | **0.99×** |
| binary-trees | 1565 | 1558 | **1.00×** |
| array | 2991 | 3145 | 1.05× |
| hash2 | 3315 | 3742 | 1.13× |
| lists | 4770 | 5423 | 1.14× |
| method-call | 2653 | 3029 | 1.14× |
| spectral-norm | 3616 | 4161 | 1.15× |
| mandelbrot | 2323 | 2728 | 1.17× |
| matrix | 2169 | 3982 | 1.84× |
| nbody | 1961 | 3773 | 1.92× |
| sieve | 1511 | 2928 | 1.94× |
| strcat | 2345 | 4606 | 1.96× |

**Geometric mean: 1.24× — madc runs at ~81% of gcc's speed** (1.29× on the
0.106.0 candidate `ded0cb01c`, 2026-10-22). `oggenc` is not in the mean:
madc refuses it (B224, a one-element row of a multi-dimensional array member;
0.105.0 refuses it the same way).

`except` was 0.51× on 2026-09-21 (`b3dc26b79`) and is 0.97× here; every
release from 0.101.0 through 0.106.0 measures ~3400 ms (B228). That one row
moves the mean from 1.19× to 1.24×.

Against the previous release, the same binaries interleaved (2026-10-23,
0.106.0 / 0.105.0): geometric mean **0.978×** over 19 C, C++ and fp
benchmarks — method-call 0.89×, hash2 and sieve 0.90×, matrix 0.95×; hash
1.04× with byte-identical instructions (code placement only).

### Read the distribution, not one benchmark

madc ties or beats gcc on five of the fifteen. The outliers are known and
documented by MIR's own author in `third_party/mir/c-benchmarks/README.md`:
*"matrix, nbody, and spectral-norm can be improved by loop-invariant motion"*
— and matrix and nbody are two of our four worst. He also notes *"except was
considerably improved by inlining"* — the September result B228 tracks.
Call-intensive benchmarks are slow because every MIR call goes through a thunk
that permits hot-swapping function code, which is a deliberate MIR design
property, not a defect.

**Quoting `sieve` alone overstates the gap by 56%.** It is in the tail of the
distribution (1.94×), and its gap is not the missing vectorizer either:
blocking gcc's vectorizer and memset idiom (`-fno-tree-vectorize
-fno-builtin`) moved gcc only 1467 → 1799 ms.

### The gap is MIR's, not madc's — controlled 2026-09-21

Stock MIR **v1.0.0** (`477d820`) was built from upstream as a clean control
and run against the same sieve source:

| path | sieve (ms) |
|---|---:|
| gcc `-O2` binary | 1435 |
| **madc `-O2` binary** | **3005** |
| in-tree c2m `-O2` `-fobject` binary | 3118 |
| in-tree c2m `-O2` `-eg` (JIT) | 3524 |
| **stock MIR v1.0.0 c2m `-O2` `-eg`** | **3533** |

Stock and in-tree MIR are **within 0.3%** of each other, so madc's fork has
not regressed codegen — and madc's own path is the *fastest* of the three MIR
routes, so neither the front end nor the AOT object writer costs anything.
Whatever the remaining gap is, it is upstream MIR's scalar code generation.

## Execution — floating point (regression sentinel)

> **Not donut.c.** donut is `for(;;)` with `usleep(30000)`: it never
> terminates and is sleep-bound, so it cannot be timed and cannot be
> output-compared (md5 of output truncated by `timeout` differs per run — it
> produced a spurious `MISMATCH` the first time this lane ran). The v0.93.0
> figure came from a modified, sleep-free 300-frame variant.
> `docs/benchmarks/fp-callchain.c` is an ORIGINAL benchmark isolating the same
> mechanism — int→double converts feeding `sin`/`cos`/`sqrt`, bounded and
> deterministic. Measured 2026-10-23 on `787813947` (argument 20000): **gcc
> 248 ms, madc 255 ms, 1.03×**, output identical. The pxor fix is holding.

**Why this benchmark exists.** MIR's x86-64 generator emitted the merging
scalar SSE converts (`cvtsi2ss` / `cvtsi2sd` / `cvtss2sd` / `cvtsd2ss`) bare,
so every convert falsely depended on the destination register's previous
writer — in sin/cos-heavy loops that is the previous libm call's return chain,
serialising calls the out-of-order core could otherwise overlap (~76 vs ~24
cycles per call). The v0.93.0 fix is the dependency-breaking idiom gcc and
clang both emit: `pxor dst,dst` before the convert. It made the donut
workload 2.8× faster (1.374s → 0.493s) and beat `gcc -O0` (0.514s).

That defect was pure upstream MIR code, unchanged since 2019, at **identical
dynamic instruction counts** — invisible to callgrind and misattributed to
libm by samplers. A silent revert, most plausibly through an upstream subtree
pull or a pattern-table edit, would pass every correctness lane we have.
**This benchmark is how we would find out**, which is why the run-this
triggers below name the MIR subtree explicitly.

## Execution — C++

`docs/benchmarks/cpp`, `g++ -O2` vs `madc -O2`, native binaries both sides.
These exercise paths the C suite cannot reach: container growth and
reallocation, virtual dispatch through a vtable, and `std::string` against
real libstdc++.

| benchmark | what it exercises |
|---|---|
| `vector-sum.cpp` | `std::vector<int>` push_back growth + indexed iteration |
| `virtual-call.cpp` | virtual dispatch through a base pointer (the vtable path) |
| `string-build.cpp` | `std::string` append + size, the libstdc++ interop path |

Measured 2026-10-23 on `787813947` (the released madc 0.106.0), argument
20000:

| benchmark | g++ (ms) | madc (ms) | madc/g++ |
|---|---:|---:|---:|
| virtual-call | 505 | 486 | **0.96×** |
| vector-sum | 818 | 996 | 1.22× |
| string-build | 104 | 207 | 1.99× |

Geometric mean 1.33×. See `history.tsv` for the tracked series.

### What the C++ numbers actually compare

**madc uses libstdc++ — the real GNU library, not an implementation of our
own.** Both binaries link the same `libstdc++.so`, so these benchmarks are not
comparing container implementations. They compare what each compiler does
*around* a shared library: how it compiles the header-defined template bodies
it parses for itself, and what it chooses to inline.

One madc-specific effect to keep in mind before reading a C++ ratio as codegen
quality: `std::string` and friends resolve **mangled-direct** against
libstdc++ (the g++ ABI, no wrapper shims — see the key design notes in
`AGENTS.md`). Where g++ inlines a header template body at `-O2`, madc may
instead emit a real call into the prebuilt shared object. That difference is a
**linkage and inlining** effect, not a statement about generated instruction
quality, and it will move if the inlining policy changes. The C benchmarks are
the cleaner read on raw codegen; these are the read on C++ interop cost.

## Compile time

`gcc -O0 -c` is the must-beat bar (`.claude/rules/gcc-parity.md`: gcc is the
performance baseline, not only the codegen one). Measured 2026-10-22 on
`ded0cb01c`, the packaged madc 0.106.0 candidate (its default, `-O1`) against
`gcc`/`g++ -O0 -c`, every compiler at its default `--std=`, best of 7,
interleaved:

**Measure with `bin/madc-release`, never `bin/madc`.** The `-O0` development
build is 1.5–2.6× slower than the release build, which is what users actually
run. Timing the dev build against gcc's release build produced a 3.76×
geometric mean that was pure artifact; `benchmark_lane.sh` now defaults to the
release binary and warns if pointed elsewhere.

**Both regimes belong in the table** — small files are startup-dominated,
large files are where the forest pays — or it tells half the story depending
on which sources were picked:

| source | gcc/g++ (ms) | madc (ms) | madc/gcc |
|---|---:|---:|---:|
| hello.c (trivial) | 10 | 6 | **0.60×** |
| sieve.c (small) | 14 | 8 | **0.57×** |
| virtual-call.cpp | 19 | 12 | **0.63×** |
| vector-sum.cpp (`<vector>`) | 80 | 51 | **0.64×** |
| string-build.cpp (`<string>`) | 96 | 57 | **0.59×** |
| hello.cpp (`<iostream>`) | 145 | 83 | **0.57×** |
| smaug `act_wiz.c` (12,041 lines) | 386 | 199 | **0.52×** |
| smaug `build.c` (10,312 lines) | 266 | 181 | **0.68×** |
| smaug `db.c` (7,936 lines) | 220 | 157 | **0.71×** |

**madc compiles every row 29–48% faster than gcc `-O0`.** The frozen header
forest earns the large-file rows: gcc re-parses the system headers on every
TU, madc does not. clang `-O0` is still faster on the large C files (act_wiz
156 ms, build 127, db 119).

Re-checked 2026-10-23 on the released `787813947` (the issue #8 fixes, and
B231: a C++ `-c` compile merges the unwind personality's prebuilt object
instead of generating it). That run's absolute times sit 10–20% above these
for gcc and clang as well as madc (host load); every madc/gcc ratio matches
to within 0.04 except virtual-call.cpp, 0.52×.

Against the previous release, the same sources interleaved (2026-10-23,
0.106.0 / 0.105.0): geometric mean **0.565×** over 40 `-c` and `-o`
compiles — C files 0.35–0.63×, C++ 0.91–0.96×, lua 5.3 (issue #8's
`lua53.c`) 170 → 152 ms.

Two traps this table records so they are not rediscovered:

- **`--std=` decides the semantics.** A `.c` file defaults to C (`gnu17`);
  a `--std=c++…` on a `.c` file puts it through the C++ path, which cost the
  SMAUG files 25–30% when it happened (`db.c` 269 ms vs 186 ms).
- **Flags precede the source path** in a madc command line.

## Real code — SQLite 3.54.0

The amalgamation (`sqlite3.c`, 271,072 lines) to an object, best of 3,
2026-10-23 on `787813947`; then the `--enable-all` `testfixture` built by each
compiler (`configure CC=…`, `-O2`, parallel make) and SQLite's
`veryquick.test` run on it:

| | madc | gcc 13.3 | clang 18.1 |
|---|---:|---:|---:|
| `sqlite3.c` → object, `-O0` | 1.9 s | 3.0 s | **1.0 s** |
| `sqlite3.c` → object, `-O2` | **3.2 s** | 16.8 s | 15.7 s |
| object size `-O0` / `-O2` | 3.6 / 2.6 MB | 1.6 / 1.4 MB | 1.6 / 1.7 MB |
| `testfixture` build | **21 s** | 36 s | 38 s |
| `veryquick.test` run | 155 s | **141 s** | **141 s** |
| `veryquick.test` result | 2 of 394,060 fail | 2 of 394,060 | 2 of 394,060 |

The two failures are the same tests under all three compilers
(`sessionnoact-4.3`, `zipfile-25.0`: the test host's Tcl, not the compiler).
madc's SQLite runs its test suite at ~91% of gcc's speed.

## When to run this

- **Every release** — both tables, appended to `history.tsv`.
- **After any change to the MIR subtree**, especially an upstream subtree pull:
  that is precisely how the v0.93.0 false-dependency fix could silently revert.
- **After front-end work that could change parse cost** — the compile table is
  the one that moves.

A regression here is a real defect, not a curiosity: gcc is the performance
baseline by rule, and `scripts/perf_vs_gcc.sh` auto-callgrinds a compile-time
outlier past its threshold.
