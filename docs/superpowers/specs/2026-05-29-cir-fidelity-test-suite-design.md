# CIR Fidelity Test Suite — Design

**Date:** 2026-05-29  **Branch:** `feature/cir-node`

## Problem

The CIR backend has 193 integration failures (`227 pass / 193 fail / 56 skip`).
Fresh triage histogram (`tmp/cir_triage.sh`):

| Count | Class | Meaning |
|------:|-------|---------|
| 96 | `c2mir_rejected` | tree builds clean; c2mir's checker rejects the emitted C11 |
| 44 | `unhandled_expr` | builder emits an error node for a token it can't translate |
| 32 | `other_nonzero` | crashes / other nonzero exits |
| 21 | `runtime_mismatch` | exit 0 but wrong output (silent miscompile) |

The dominant class — `c2mir_rejected` (50%) — is **opaque** today: the only
signal is `cir_compile failed`. There is no indication of *which* node or
construct c2mir rejected, so each failure is an open-ended investigation. The
21 `runtime_mismatch` are silent miscompiles with no localization at all. This
suite makes those failures **mechanical and localized** — point at the exact
function / node that is wrong.

## Key enabler: the MC11-IR is both lowered and high-level

The `cir_node` tree **IS-A** c2mir `node_t` *and* carries each node's
originating tokens (see `docs/rules/mc11-ir.md`). This design exploits two
consequences:

1. We can **render the tree back to C** (the `.c` / `.mc11` renderer).
2. We can feed our tree to **c2mir's own node printer**, getting a dump that is
   byte-comparable to `c2m -d`.

## Components

### 1. Renderer — `cir_node → text`, new flag `--emit=c11|mc11`

- `--emit=c11` → plain C11 (madc metadata stripped). `--emit=mc11` → C11 +
  `madc`-namespaced pragmas (`_Pragma("madc …")` / `#pragma madc …`) carrying
  the extra info. Strip the metadata → `c11` (the strip-to-`.c` invariant).
  Both render from the one MC11-IR. Replaces the currently-erroring `--emit-c`.
- The vestigial `--backend` flag is removed in the same arg-parsing edit (CIR is
  the sole backend; the suite invokes bare `bin/madc`).
- **Implementation:** c2mir has **no** C-regenerator (it only parses
  C → nodes → MIR; `print_node` at `c2mir.c:14319` is a debug *dumper*). So we
  write our own `node_t → C` pretty-printer that walks the `cir_node` tree and
  emits compilable C11, mirroring the structure of `print_node` but emitting C
  syntax. Target selection is the shared `Lang` enum (`C11`, `MC11`; `Cpp` /
  `Madc` reserved as future values — not built now, per YAGNI).

### 2. Fidelity gate — `gcc -S` differential (sharpest for plain C)

For an input source `S`:

```
asm_orig     = gcc -S -fverbose-asm -O0  S   -o -
S.c          = bin/madc --emit=c11  S
asm_emitted  = gcc -S -fverbose-asm -O0  S.c -o -
diff: per-function, label-normalized, asm_orig vs asm_emitted
```

For plain-C input the emitted C should compile to **near-identical** assembly;
any divergence localizes the construct madc lowered incorrectly. Normalization
strips `.L<n>` labels, `.file`/`.ident` directives, and addresses so only
semantic differences remain. This is the "transpiler fidelity" leg of the
differential triangle: `gcc(original) ≈ gcc(.mc11/.c) ≈ c2mir(tree)`.

### 3. Tree differential — `--dump-cir` vs `c2m -d`

Because `cir_node` IS-A `node_t`, feed our tree to c2mir's `print_node` /
`debug_node` so `--dump-cir` emits the **same format** as `c2m -d`. Then:

```
bin/madc --dump-cir  S      → tree_madc
c2m -d               S.c    → tree_c2m
diff tree_madc vs tree_c2m  → exactly where the cir_node tree diverges
```

This directly explains `c2mir_rejected` failures (e.g. "struct has no member
name", "lvalue required") by showing the node-shape mismatch. (Confirm/adjust
the current `--dump-cir` to route through c2mir's printer for format parity.)

## Data flow

```
input.mad → lexer/parser → cir_node tree (MC11-IR)
   ├─ --emit=c11 → S.c → gcc -S ─┐  fidelity gate
   │  original S      → gcc -S ──┴─ per-fn label-normalized asm diff
   ├─ --dump-cir (via c2mir print_node) ─┐  tree differential
   │  S.c → c2m -d ──────────────────────┴─ text diff
   └─ (normal run) → c2mir → MIR → exec → .expect   (existing pass/fail)
```

## Harness & corpus

- `scripts/cir_fidelity.sh <test>` — runs the gate + tree differential for one
  input and prints the localized diff(s). Generic, fixture-driven (no per-test
  branching, per `test-fixtures.md`).
- A `make` target (e.g. `cirfidelity`) or a `--all` mode runs a corpus and
  summarizes which tests diverge and where.
- **Corpus:** the 96 `c2mir_rejected` tests first (most localizable), plus small
  plain-C reducers in `tmp/` where the gcc leg is sharpest. Reuses existing
  `tests/*.mad` + fixtures.

## Error handling / edge cases

- **Non-plain-C input** (C++/madc constructs lowered to C): the gcc-fidelity
  leg cannot expect identity (gcc won't compile the lowered form the same as the
  C++ original). Detect via gcc exit on `S.c`; when it isn't standalone-
  compilable, **skip the gcc leg** and rely on the tree differential + the run.
- **Normalization robustness:** label/address stripping must be deterministic.
- **Pragma inertness:** `#pragma madc` / `_Pragma("madc …")` are ignored by gcc
  and c2m (already verified byte-identical) — so `mc11` and `c11` produce the
  same assembly; the gate uses `c11`.

## Testing the suite itself

- A few golden plain-C reducers with known-good emitted `.c`; assert the gate
  reports clean.
- The suite is **diagnostic tooling**, not initially a CI pass/fail gate: it
  localizes a failure; we then fix the renderer/builder; the existing
  integration suite confirms the fix.

## Build sequence

1. `--emit=c11` renderer (`node_t → C` printer) + remove `--backend`.
2. `scripts/cir_fidelity.sh` gcc-fidelity leg on 3–5 plain-C reducers — prove
   the loop end-to-end.
3. Route `--dump-cir` through c2mir's `print_node`; add the tree-diff leg.
4. `make` target + run over the 96 `c2mir_rejected`; triage the localized output
   into the next round of renderer/builder fixes.

## Open / deferred

- **MC11 metadata schema** (which pragmas carry what): minimal at first — only
  enough for a faithful round-trip. Expand when reverse-rendering to C++/madc
  is built (future `Lang` enum values). Not in scope now.
- **AOT / eval / REPL:** unrelated; remain deferred.
