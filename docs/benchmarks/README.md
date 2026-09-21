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
`madc -O2`, native binaries both sides. Measured 2026-09-21 on `b3dc26b79`,
20-core x86-64 container, gcc 13.3.0.

| benchmark | gcc (ms) | madc (ms) | madc/gcc |
|---|---:|---:|---:|
| except | 3487 | 1775 | **0.51×** |
| heapsort | 4791 | 4707 | 0.98× |
| hash | 3658 | 3618 | 0.99× |
| binary-trees | 1463 | 1467 | 1.00× |
| funnkuch-reduce | 1809 | 1901 | 1.05× |
| spectral-norm | 3490 | 4014 | 1.15× |
| mandelbrot | 2233 | 2646 | 1.18× |
| array | 2303 | 2737 | 1.19× |
| method-call | 2517 | 3059 | 1.22× |
| hash2 | 3040 | 3801 | 1.25× |
| lists | 4563 | 5788 | 1.27× |
| nbody | 1889 | 3648 | 1.89× |
| matrix | 2119 | 3996 | 1.89× |
| strcat | 2167 | 4332 | 2.00× |
| sieve | 1456 | 2990 | 2.05× |

**Geometric mean: 1.24× — madc runs at ~81% of gcc's speed.**

### Read the distribution, not one benchmark

madc ties or beats gcc on a third of the suite and is *twice as fast* on
`except`. The outliers are known and documented by MIR's own author in
`third_party/mir/c-benchmarks/README.md`: *"matrix, nbody, and spectral-norm
can be improved by loop-invariant motion"* — and matrix and nbody are two of
our three worst. He also notes *"except was considerably improved by
inlining"* — our best result. Call-intensive benchmarks are slow because every
MIR call goes through a thunk that permits hot-swapping function code, which
is a deliberate MIR design property, not a defect.

**Quoting `sieve` alone overstates the gap by 65%.** It is the tail of the
distribution (2.05×), and its gap is not the missing vectorizer either:
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

`donut.c` is call-heavy floating point and exists here for one reason: it is
the sentinel for the **v0.93.0 MIR false-dependency fix**. MIR's x86-64
generator emitted the merging scalar SSE converts (`cvtsi2ss` / `cvtsi2sd` /
`cvtss2sd` / `cvtsd2ss`) bare, so every convert falsely depended on the
destination register's previous writer — in sin/cos-heavy loops that is the
previous libm call's return chain, serialising calls the out-of-order core
could otherwise overlap. The fix is the dependency-breaking idiom gcc and
clang both emit: `pxor dst,dst` before the convert.

It made donut **2.8× faster (1.374s → 0.493s) and beat `gcc -O0` (0.514s)**.

That defect was pure upstream MIR code, unchanged since 2019, and it was
invisible to callgrind (identical dynamic instruction counts) and
misattributed to libm by samplers. A silent revert — an upstream subtree pull,
a pattern-table edit — would not show up in any correctness lane. **This
benchmark is how we would find out.**

`donut.c` is third-party source and is **not vendored**: the lane runs it when
the owner's copy is present at the repo root and skips it otherwise.

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

Numbers are produced by the lane; see `history.tsv` for the tracked series.

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
performance baseline, not only the codegen one). Measured 2026-09-21 on
`b3dc26b79`:

| source | gcc/g++ (ms) | madc (ms) | madc/gcc |
|---|---:|---:|---:|
| hello.c (trivial) | 12 | 28 | 2.33× |
| hello.cpp (`<iostream>`) | 132 | 103 | **0.78×** |
| smaug `act_wiz.c` (12,041 lines) | 400 | 246 | **0.61×** |
| smaug `build.c` (10,312 lines) | 293 | 227 | **0.77×** |
| smaug `magic.c` (7,394 lines) | 238 | 183 | **0.77×** |
| smaug `db.c` (7,936 lines) | 231 | 186 | **0.81×** |
| smaug `act_info.c` (6,076 lines) | 197 | 161 | **0.82×** |

**On real C translation units madc compiles 20–40% faster than gcc `-O0`** —
the frozen header forest earns that: gcc re-parses the system headers on every
TU, madc does not. The trivial-file gap is fixed startup cost, tracked
separately by the cold-startup arc (bar: tinycc parity, ~22 ms).

Two traps this table records so they are not rediscovered:

- **`.c` does not select C mode.** madc chooses semantics from `--std=`, not
  from the file extension. Compiling the SMAUG files without `--std=c17` put
  them through the C++ path and cost 25–30% (`db.c` 269 ms vs 186 ms).
- **Flags precede the source path** in a madc command line.

## When to run this

- **Every release** — both tables, appended to `history.tsv`.
- **After any change to the MIR subtree**, especially an upstream subtree pull:
  that is precisely how the v0.93.0 false-dependency fix could silently revert.
- **After front-end work that could change parse cost** — the compile table is
  the one that moves.

A regression here is a real defect, not a curiosity: gcc is the performance
baseline by rule, and `scripts/perf_vs_gcc.sh` auto-callgrinds a compile-time
outlier past its threshold.
