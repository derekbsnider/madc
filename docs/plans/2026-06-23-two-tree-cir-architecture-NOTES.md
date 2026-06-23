# Two-Tree cir_node Architecture — REFERENCE NOTES (pre-plan)

**Date:** 2026-06-23 · **Branch:** `feature/front-end-performance-claude`
**Status:** NOTES + cross-check scratchpad. The PLAN is written only after the
g++ / c2mir cross-checks below confirm the model jives.

> Origin: design-owner directive (2026-06-23). Supersedes token-arena step 2.3
> (the scratch-token re-parse isolation) — we pivot to the g++ model at the TREE
> level instead of optimizing the re-parse madc currently does.

## The model (design owner)

Two `cir_node` AST trees, the same ROM/RAM split we built at the token level,
lifted to the tree level:

- **Tree 1 — STATIC / IMMUTABLE / EMBEDDABLE ("the saved tree").**
  Parsed once: the system/embedded headers (the whole corpus) + every template
  PATTERN. Never mutated. Serialized/embedded → this IS the header-forest.
  Shared across compiles. g++ analogue: `DECL_SAVED_TREE` of a template pattern,
  generalized to the entire header corpus.
- **Tree 2 — DYNAMIC / MUTABLE / SEMA ("the brand-new tree").**
  The per-translation-unit `cir_node` tree that is handed to c2mir. Built by
  **copy + substitute** (`tsubst`) from Tree 1. Carries sema results. Unchanged
  subtrees **point back into Tree 1 by index** instead of being copied.

Instantiation and the forest become ONE mechanism: Tree 1 = immutable ROM of
trees; Tree 2 = mutable RAM tree that references Tree 1 and feeds c2mir.

### How this sits on what's already built (token level)
- pop-1 lexed tokens: immutable POD records (ROM), complete as of `c76e59a`.
- pop-2 / parse: references pop-1 by slot-id.
- `cir_node.origin_id` ALREADY links a node down to its token by INDEX (slot-id),
  not a pointer (`src/cir_node.h:46`). So the **token→tree** back-ref exists.
- MISSING: the **tree→tree** back-ref. A `cir_node`'s CHILDREN are still c2mir's
  pointer-linked DLIST (`NL_HEAD`/`NL_NEXT`), not arena indices. The cir_node
  arena (immutable Tree-1 space + mutable Tree-2 space, children = indices) is
  the next track to build.

## CROSS-CHECK QUESTIONS (must answer before writing the plan)

### g++ (gcc/cp/pt.cc)
- [Q-G1] Is the saved tree (`DECL_SAVED_TREE`) truly immutable during instantiation?
- [Q-G2] Does `tsubst` build FRESH nodes (copy), or share unchanged subtrees? If it
  shares, what does it share (types? exprs?) and how does it stay safe?
- [Q-G3] What exactly is handed to the back end — the per-instantiation copy
  (GENERIC), never the pattern?

### c2mir (/workspace/mir/c2mir/c2mir.c)
- [Q-C1] **CRITICAL:** does c2mir MUTATE the `node_t` tree during compilation
  (annotate nodes with types/attrs in place, rewrite, etc.)? If yes, then Tree-2
  fed to c2mir must be safe to mutate — and any subtree it SHARES with immutable
  Tree-1 would be corrupted. → the "point back to Tree-1" sharing must be resolved
  (materialized / copy-on-write) on whatever c2mir touches, BEFORE c2mir runs.
- [Q-C2] Which node fields does c2mir write? (Determines what must be private in
  Tree-2 vs. what can stay shared from Tree-1.)
- [Q-C3] Does c2mir consume the tree once per compile (so Tree-2 is disposable)?

## Findings (recon 2026-06-23)

### g++ (gcc/cp/pt.cc) — VERIFIED
- [Q-G1] YES. The pattern's `DECL_SAVED_TREE` is the immutable source; instantiation
  reads it. `instantiate_decl` starts the fn with `SF_PRE_PARSED` (pt.cc:28695) then
  `tsubst_stmt(DECL_SAVED_TREE(code_pattern), args, …)` (28709). The pattern is NOT
  re-parsed and NOT mutated — it is substituted FROM.
- [Q-G2] tsubst COPIES into fresh nodes — `copy_node`/`tsubst_copy` pervasive
  (pt.cc:3859, 5024, 8247…). It does not splice the pattern's live statement/expr
  nodes into the instantiation. (Canonical TYPE nodes are shared because they're
  immutable/canonical — not relevant to the mutable body.)
- [Q-G3] The back end sees the per-instantiation COPY (GENERIC), never the pattern.

### c2mir (/workspace/mir/c2mir/c2mir.c) — VERIFIED, and it CONSTRAINS the design
- [Q-C1] **YES — c2mir mutates the node tree in place.** Context-checking attaches
  all sema (scope/decl/type/const-expr) to each node via `node->attr`
  (`n->attr = curr_scope` etc.: c2mir.c:5466,5572,5611,…; read back at 6366,6373,
  6840,6940…). `n->attr` is NULLed only at node creation (697).
- [Q-C2] The hot mutated field is `attr` (the sema annotation). Structure (code,
  children) is read; `attr` is written per node. **SCOPE (design owner, 2026-06-23):
  c2mir only understands the `node_t`/`struct node` shape that `cir_node` embeds at
  offset 0. It cannot see or touch ANY `cir_node` extension field, nor anything
  those fields point to. ⇒ c2mir's mutation is CONFINED to the node_t base. The
  madc extension area — including a back-reference to Tree-1 — is invisible to c2mir
  and thus inherently safe from it (same property that makes `origin_id` safe).**
- [Q-C3] One compile pass per TU; each node compiled once. Two instantiations that
  shared a live node would have c2mir write conflicting `attr` on it → corruption.

## Verdict — IT JIVES (with one refinement c2mir forces)

The two-tree model is sound AND matches both g++ and c2mir, with this precise rule:

- **Tree-1 (immutable corpus + template patterns)** is the COPY SOURCE and the
  serialized/embedded form (the forest). It is never handed to c2mir and never
  mutated. Exactly g++'s `DECL_SAVED_TREE`, generalized to the whole header corpus.
- **Tree-2 (per-TU, → c2mir)** is built by `tsubst` = **copy Tree-1 subtrees into
  FRESH nodes + substitute params**. Because c2mir writes `attr` on every node it
  compiles, every node in the c2mir-bound tree must be PRIVATE — so instantiation
  COPIES (it cannot splice live Tree-1 nodes, and two instantiations cannot share a
  live node). This is exactly what g++ does (`copy_node` in tsubst).
- **"Point back to Tree-1 by index"** lives in the `cir_node` EXTENSION area
  (invisible to c2mir — design owner), so a Tree-2 node may safely hold a reference
  to its immutable Tree-1 source even while c2mir compiles it; c2mir never follows
  or mutates that link. What MUST be private per instantiation is the **node_t base
  structure** (c2mir writes `attr` on each node_t it traverses). So:
  - **node_t base (+ the child structure c2mir traverses) = RAM**, materialized
    fresh per instantiation in Tree-2 (the part c2mir scribbles on).
  - **the rich madc info (datadef detail, the link to the pattern node) = ROM**,
    referenced from Tree-1 via the extension; only the SUBSTITUTED/differing fields
    are fresh in Tree-2. ("Some ROM, some RAM, not all RAM" — at the tree level.)
  The realized WIN is eliminating RE-PARSE (build the small node_t structure by
  copy+substitute ≪ re-lex+re-parse+re-sema tokens) — identical to g++'s rationale.

⇒ token-arena step 2.3's "scratch tokens to make re-parse safe/cheap" is the wrong
layer (it optimizes the re-parse g++ doesn't do). Superseded. Plan: sibling doc
`2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md`.
