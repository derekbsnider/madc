# Clang Parity — Reasoning

When native EXE output diverges from JIT output, there are usually two
possible failure classes:

1. madc emitted the wrong machine-code shape in the first place, or
2. madc emitted a workable JIT shape but exported it incorrectly to the
   ELF/AOT path.

Clang is the fastest way to separate those cases.

## Why compare against clang first

Clang provides a stable external baseline for ordinary C/C++ lowering:

- whether a struct copy is inlined or lowered through a helper call
- whether a pointer cast/deref becomes a load, LEA, or plain move
- whether a constant or symbol reference should live in code, data, GOT,
  or PLT space

That baseline does not dictate madc semantics, but it does show whether
madc is creating unnecessary complexity in a hot path. When clang uses a
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

- Clang is a reference, not an authority over madc language semantics.
- madc may still diverge deliberately for language features clang does not
  model.
- Do not cargo-cult clang output; use it to sharpen the hypothesis before
  editing.
