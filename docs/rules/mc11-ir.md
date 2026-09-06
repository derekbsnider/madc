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

## Why a token copy keeps its origin (2026-09-04)

`TokenBase::clone()` and its 118 overrides construct a fresh token, and the
TokenBase constructor stamps it with the static `_parse_file/_line/_column` —
the position of the token the parser last consumed. Every parser-side copy of a
template pattern, a default argument, a call argument or a substitution run was
therefore attributed to wherever the parser happened to be at copy time. The
consequence was not cosmetic: the builder's root/library split classifies a
function body by its file, so an instantiated member-template body attributed
to the user's `.mad` was a ROOT — emitted unconditionally — while the same body
attributed to the header was a library function emitted only on reference.
Live parses got the header by accident (the last token consumed before the
injection came from enable_if.h); the darwin pack lane got the user file, and
lowered an unselected libc++ `basic_string` constructor instance whose helper
called a never-instantiated member template: an undefined import on both mac
arches (dispatch #8). Two earlier fixes had met the same stamping and patched
around it locally by redirecting `_parse_*` across a clone loop (class-template
instantiation, the lazy pattern capture); the member-template injection had
neither — the rule lived in N places.

gcc locates an instantiation at the template's definition (the instantiated
decl carries the pattern's DECL_SOURCE_LOCATION); clang the same (the
template's SourceLocation). A macro-expanded token, by contrast, is located at
its expansion site (gcc's primary location) — so the lexer's replacement clones
stay bare, and `src/lexer.cpp` is the gate's one exempt file. The two
`_parse_*` redirects remain: they still attribute the tokens those loops
SYNTHESIZE (`new TokenIdent(mangled)`, concrete type tokens) to the template.
