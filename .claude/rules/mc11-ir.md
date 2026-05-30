# MC11-IR — The Mad-enhanced-C11 Intermediate Representation

- The primary in-memory representation is the `cir_node` AST tree: the
  Mad-enhanced-C11 IR (MC11-IR). It is the single source of truth.
- `cir_node` DERIVES FROM c2mir's `node_t`. It is not a separate parallel
  AST — do not invent or revive a competing tree type.
- Every `cir_node` carries its originating lexed tokens and the `TokenBase`
  parse subtree that built it.
- Those tokens carry file / line / column. Preserve them — they are the path
  back to the original source.
- The IR is BOTH lowered and high-level by construction:
  - c2mir consumes the lowered C11 `node_t` view (classes already
    struct + functions, templates already instantiated, etc.).
  - madc reads the retained high-level structure from the attached tokens +
    parse subtree + source positions.
- Reverse-rendering to C++ / madc / original source reads the attached
  tokens and parse subtree — never reconstruct high-level constructs from
  emitted comments or pragmas.
- `.mc11` text is the serialization of this extra info; the live tree already
  holds the real structures.
- All render targets (C11, MC11, C++, madc, …) read this one IR, selected via
  the shared language-target enum (the same enum that drives `--std=`).
- Never re-pose "should the cir_node tree be lowered OR high-level." It is
  deliberately both.

See `docs/rules/mc11-ir.md` for the reasoning.
