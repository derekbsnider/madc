# MC11-IR — Reasoning

## What the IR is

madc's primary in-memory representation is the **Mad-enhanced-C11
Intermediate Representation (MC11-IR)** — the `cir_node` AST tree built by
`CirBuilder` (`src/cir_builder.cpp`). Two properties define it, and they are
the whole point:

1. **`cir_node` derives from c2mir's `node_t`.** The tree is c2mir-friendly
   not by translation at the boundary but by *construction* — it already is
   the shape c2mir consumes. There is no separate "madc AST" that then gets
   converted into a "c2mir AST"; it is one tree.

2. **Each `cir_node` carries its provenance.** Riding along with every node
   are the originating lexed tokens and the `TokenBase` parse subtree that
   produced it, and those tokens carry `file` / `line` / `column`. The IR
   therefore always knows where it came from and can walk back to the
   original source text.

## Why this resolves the "lowered vs high-level" question

When you lower C++/madc to C for c2mir, `class Dog { void bark(); }` becomes
`struct Dog;` + `Dog__bark(struct Dog*)`. A naive design must choose:

- **Lower late** (keep `class` in the tree, flatten only at hand-off) — easy
  to render back to C++, awkward for c2mir.
- **Lower early** (store only the flattened form) — easy for c2mir, but the
  fact "this was a class" is gone, so reverse-rendering needs sticky-note
  metadata to reconstruct it.

MC11-IR refuses the choice. From **c2mir's** perspective the tree is the
lowered form (B): it reads the `node_t` view directly. From **madc's**
perspective the original high-level structure is still present (A): not
reconstructed from comments, but *retained* in the attached tokens and parse
subtree. One tree, both views, no rebuild.

## Consequences

- **Reverse-rendering** to C++ / madc / original source reads the attached
  tokens and parse subtree — it does not parse emitted comments or pragmas to
  guess the high-level shape.
- **`.mc11` text** (C11 + `madc`-namespaced pragmas/comments) is the
  *serialization* of the extra info for on-disk round-trips. The live tree
  already holds the real structures; `.mc11` is how they survive a write/read
  cycle when the tokens aren't in memory.
- **The render-target enum** (C11, MC11, C++, madc, …) — the same enum that
  selects `--std=` — picks which view of the one IR to emit. C11/MC11 render
  the lowered view; C++/madc render from the retained tokens+parse subtree.
- **Source positions must be preserved** through every transform. Dropping
  `file`/`line`/`column` severs the path back to source and breaks the A view,
  diagnostics, and the IDE operating modes.

## Set in stone

This was dictated by the project owner on 2026-05-29 as a fixed architectural
decision. Do not re-open "should the cir_node tree be lowered or high-level" —
it is deliberately both, by deriving from `node_t` while carrying its
originating tokens.
