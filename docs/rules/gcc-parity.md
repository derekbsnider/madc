# GCC Parity — Reasoning

When native EXE output diverges from JIT output, there are usually two
possible failure classes:

1. madc emitted the wrong machine-code shape in the first place, or
2. madc emitted a workable JIT shape but exported it incorrectly to the
   ELF/AOT path.

GCC is the fastest way to separate those cases.

## Why compare against GCC first

GCC provides a stable external baseline for ordinary C/C++ lowering:

- whether a struct copy is inlined or lowered through a helper call
- whether a pointer cast/deref becomes a load, LEA, or plain move
- whether a constant or symbol reference should live in code, data, GOT,
  or PLT space

That baseline does not dictate madc semantics, but it does show whether
madc is creating unnecessary complexity in a hot path. When GCC uses a
simple inline lowering and madc uses an external helper call, the helper
path becomes a likely parity risk and a likely simplification target.

## How to use the comparison

For parity work:

1. Reduce the failing `.mad` case to the closest valid GCC analogue.
2. Compile it with low optimization first (`-O0`) so the emitted shape
   is easy to inspect.
3. Inspect the relevant assembly around the failing operation.
4. Decide whether the bug belongs in madc codegen or in the AOT/export
   path.

## What this rule does not mean

- GCC is a reference, not an authority over madc language semantics.
- madc may still diverge deliberately for language features GCC does not
  model.
- Do not cargo-cult GCC output; use it to sharpen the hypothesis before
  editing.

## GCC as the performance baseline

GCC does strictly more work than madc's front-end on the same translation
unit (it also optimizes and emits an object), yet it is heavily tuned. So if
madc's *parse + c2mir* time exceeds GCC's *whole `-O0 -c`* time, madc is almost
certainly doing something algorithmically wrong, not merely "unoptimized" — the
canonical example is an unbounded-lookahead helper that scans to end-of-stream
once per token (O(n²)). That class is invisible on small inputs and only shows
up on large or macro-heavy bodies (gcc.c-torture `memcpy-a*`, `memclr`).

The standing practice: **whenever a test is slower than GCC, callgrind it** and
read the top self-cost madc function. `scripts/perf_vs_gcc.sh` automates the
compare-and-callgrind; `docs/plans/2026-06-23-parser-lookahead-audit.md` is the
running log of the bug class and the per-site triage worklist. tinycc
(`/workspace/tinycc`) is a useful *floor* reference (interned int tokens,
pre-tokenized macro bodies) for how fast this can ultimately get; GCC is the
must-beat bar.
