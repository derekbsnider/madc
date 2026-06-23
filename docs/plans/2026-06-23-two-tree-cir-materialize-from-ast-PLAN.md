# Two-Tree cir_node / Materialize-from-AST — PLAN

**Date:** 2026-06-23 · **Branch:** `feature/front-end-performance-claude`
**Status:** GOVERNING PLAN. Supersedes token-arena step 2.3 (scratch-token re-parse
isolation). Cross-checked against g++ and c2mir — see
`2026-06-23-two-tree-cir-architecture-NOTES.md` (the verified findings this plan
rests on; read it first).

> Design owner directive (2026-06-23): adopt the **g++ model** — parse a template
> body (and the whole system-header corpus) ONCE into an immutable tree, and
> instantiate by **copy + substitute** of that tree, NEVER re-parsing. The two
> trees are the token-level ROM/RAM split lifted to the `cir_node` level.

---

## 1. THE GOAL (one sentence)

Stop re-parsing template bodies per instantiation; instead parse each body once
into an immutable `cir_node` pattern (Tree-1) and instantiate by copying that
subtree into a fresh per-TU mutable `cir_node` tree (Tree-2) with template params
substituted — the same thing g++ does (`tsubst` over `DECL_SAVED_TREE`), and the
~4× / 87%-of-parse-time lever named in the front-end-perf plan §5.

## 2. THE TWO TREES (verified model)

- **Tree-1 — immutable / static / embeddable (the "saved tree").** Parsed once:
  every template PATTERN + (eventually) the whole embedded-header corpus. Never
  mutated, never handed to c2mir. It is the COPY SOURCE for instantiation and the
  SERIALIZED form (this is the header-forest). g++ analogue: `DECL_SAVED_TREE`,
  generalized to the corpus.
- **Tree-2 — mutable / dynamic / sema (the c2mir tree).** The per-translation-unit
  `cir_node` tree handed to c2mir. Built by `tsubst` = **copy Tree-1 subtree into
  FRESH nodes + substitute params**. Carries madc sema; c2mir then writes its own
  sema (`node->attr`) onto it in place.

**Hard constraint from c2mir (verified), bounded by the node_t/extension split:**
c2mir only understands the `node_t` (`struct node`) shape that `cir_node` embeds at
offset 0 — it cannot see or mutate any `cir_node` extension field or anything those
fields point to. Its mutation (`node->attr`, `c2mir.c:5466…`) is therefore CONFINED
to the node_t base. Consequences:
- **The node_t base structure that c2mir traverses must be PRIVATE per instantiation**
  (c2mir writes `attr` on each node_t). So `tsubst` builds the node_t structure
  FRESH (copy + substitute) — it never splices live Tree-1 node_t's, and two
  instantiations never share a live node_t. (Exactly why g++ `tsubst` uses
  `copy_node`.)
- **The back-reference to Tree-1 lives in the `cir_node` EXTENSION** (invisible to
  c2mir, like `origin_id`). A Tree-2 node may safely point at its immutable Tree-1
  source while c2mir compiles it. So the heavy madc info (datadef detail, the
  pattern-node link) stays ROM in Tree-1 and is referenced; only the node_t base
  and the SUBSTITUTED fields are fresh RAM in Tree-2 ("some ROM, some RAM").
The realized win is eliminating RE-PARSE; building the small node_t structure by
copy+substitute is cheap next to re-lex+re-parse+re-sema.

## 3. WHAT EXISTS (foundation already landed)

- pop-1 tokens are immutable POD records (ROM); pop-2 references them by slot-id.
  Token records are complete as of `c76e59a` (kind/value/spelling/provenance).
- `cir_node` (`src/cir_node.h`) derives from c2mir `node`; it links DOWN to its
  token by INDEX already (`origin_id` = slot-id, not a pointer). So token→tree
  back-reference is done.
- `cir_builder.cpp` lowers the parsed tree → `cir_node` tree → c2mir.

**What is NOT built (the gap):**
- A `cir_node`'s CHILDREN are still c2mir's pointer-linked DLIST (`NL_HEAD`/
  `NL_NEXT`), not arena indices ⇒ no indexable cir_node arena, no tree→tree
  index reference, no serializable tree yet.
- Instantiation today happens at the PARSER/TOKEN level: it copies body TOKENS,
  substitutes, splices, and RE-PARSES (`instantiate_template_use`, parser.cpp:3469).
  The cir_node tree is built AFTER instantiation, from the already-instantiated
  parse tree. The g++ model moves instantiation to the cir_node (tree) level.

## 4. THE HARD PART (name it honestly)

A template PATTERN tree contains UNRESOLVED dependent types (`T`, `T::type`,
`sizeof...(Ts)`). madc currently resolves types DURING parse, so it has no notion
of a parsed-but-dependent tree. g++ represents these as `TEMPLATE_TYPE_PARM` /
dependent nodes in the saved tree and resolves them in `tsubst`. **madc must gain
the same:** a `cir_node` pattern where template params are placeholders, plus a
`tsubst` that resolves them per instantiation. This is the core new machinery and
the bulk of the work (g++'s `tsubst*` is thousands of lines). The plan is therefore
incremental, widening coverage one construct at a time, with the existing re-parse
path as the fallback until tree-`tsubst` subsumes it.

## 5. COEXISTENCE / MIGRATION (no-parallel-implementations discipline)

- The re-parse path (`instantiate_template_use`) STAYS as the fallback while
  tree-`tsubst` coverage grows. New path is selected per-instantiation by a
  capability check ("can tsubst handle this pattern?"); otherwise fall back.
- This A/B coexistence is allowed ONLY because it carries a written deletion
  trigger: **when tree-`tsubst` covers the full container/torture suite, delete the
  re-parse instantiation path** (and prove it dead via `-Wunused-function`). Until
  then the fallback runs in CI every commit (the gate), so it is not dead code.
- Tests always exercise the production entry (`bin/madc`); both paths gated.

## 6. PHASES (each phase = several gated commits; gate = §8)

**Phase 0 — audit the instantiation/cir seam (recon, no code). — DONE 2026-06-23.**

RESULTS (audited; file:line evidence):
- **cir build is post-parse, not a token re-walk.** `CirBuilder::translate_module(Program*)`
  (`cir_builder.cpp:12256`, called from `madc_cir.cpp:205`) consumes the fully-parsed
  `Program`: `prog->pending_funcs` (each a `TokenFunc`, which IS-A `TokenCpnd` with
  `statements`/`variables`) + `prog->top_decls`. Per function: `func_def(tf)` →
  `translate_block((TokenCpnd*)tf)` (`cir_builder.cpp:11384`) lowers the PARSED body to
  cir_node. User bodies are eager `TokenCpnd`; system-header bodies are deferred raw
  token vectors (`DeferredFunctionBody.body_tokens`) re-parsed on demand.
- **Instantiation today is token-level clone + RE-PARSE** (`instantiate_template_use`,
  `parser.cpp:3469`): `TemplateDef.body` is a frozen `vector<TokenBase*>`; it clones the
  tokens substituting param-name `TokenIdent`s → concrete `TokenDataType` clones, splices
  into the parse stream, and RE-PARSES (`TokenCLASS::parse` / `parseFunction`), producing a
  `DataDefCLASS` / `TokenFunc` in `pending_funcs`. **cir_nodes are built LATER** in
  `translate_module`, from those concrete parsed structures — never during the re-parse.
  Fn templates: `try_instantiate_namespace_fn_template` (`parser.cpp:31102`), same pattern.
- **copy_cir_subtree recipe (Phase 1):** per node — `arena.alloc()`; copy
  `base.code`/`base.u`/`origin_id`/`datadef`/`typedef_name`/`src_lang`/`synth_from_origin`;
  assign a FRESH `uid` (`c2mir_next_uid` — uids must be unique per c2m ctx); call
  `c2mir_init_node_ops()`; recurse children over the `base.ops` DLIST (`NL_HEAD`/`NL_NEXT`)
  and re-`c2mir_op_append` each copy; re-intern any `base.u.s.s` string via
  `c2mir_uniq_str`. Do NOT copy `attr` or `error_msg` (post-check artifacts). Builders to
  mirror: `CirBuilder::make` (`cir_builder.cpp:69`), `append` (:316), `node1..4` (:321-358).

**THE CRUX GAP (decides the placeholder representation):** there is **no `DataDefTemplateParam`
/ `dtDEPENDENT`** — madc never represents `T` as a typed placeholder. In a template body `T`
is a bare `TokenIdent("T")`, replaced at the TOKEN level *before* the parser sees it. The only
dependent concept is `DataDefCLASS::is_dependent_placeholder` (`datadef.h:819`) for unresolved
instantiation RESULTS, not for `T` itself; fn templates keep `FuncDef::template_param_names` as
strings (`madc.h:175`). ⇒ A cir_node PATTERN containing `T` cannot be lowered today —
`translate_expr` would call `dd->size()`/`rawtype()` on `T` and crash/mis-lower.

**SEAM:** `CirBuilder::func_def(tf)` at the `translate_block` call (`cir_builder.cpp:11384`) is
where a body becomes cir_node — the point to (cache-miss) build & cache Tree-1, or (cache-hit)
copy+substitute into Tree-2. `tf->var.type` (`FuncDef*`) carries the template identity to key
the cache. But this REQUIRES the placeholder infrastructure first.

**Sequencing consequence (folded into the phases below):** a NEW prerequisite phase —
**Phase 1.5: a `DataDef` template-parameter placeholder** that `cir_builder` + the type-lowering
helpers tolerate and that `tsubst` substitutes — sits between the copy primitive (Phase 1) and
pattern-lowering (Phase 2). Phase 1 (`copy_cir_subtree` on a CONCRETE tree) is independent of
this gap, so it remains the correct safe first slice.

**Phase 1 — cir_node deep-copy primitive (`tsubst` core, no substitution yet).**
- `copy_cir_subtree(node)` → a tree of FRESH nodes with a fresh node_t base
  (node_t `attr`/c2mir state cleared, children rebuilt as fresh node_t links so
  c2mir traverses only private node_t's), `origin_id` preserved, and a NEW extension
  back-reference recording the source (Tree-1) node it was copied from (invisible to
  c2mir; for provenance/dedup/serialization). The heavy madc info may be referenced
  from the source rather than duplicated; only fields that will be substituted are
  owned by the copy. This is the safe-for-c2mir private materialization.
- Add the back-ref extension field to `cir_node` (a `uint32` pattern-node index, the
  tree-level analogue of `origin_id`). Until the cir_node arena exists (Phase 6), it
  can be a pointer; design it as an index.
- Prove it: copy a normal (non-template) function's cir_node subtree, compile the
  copy instead of the original, byte-identical output. Gated.

**Phase 1.5 — template-parameter placeholder DataDef (NEW prerequisite; Phase 0 finding).**
- Introduce a `DataDef` kind representing an unresolved template parameter `T`
  (a real placeholder, not a string). Make the type-lowering helpers
  (`size()`/`rawtype()`/`is_*`/`datadef()` users in `cir_builder.cpp` and the
  parse-time type resolvers) TOLERATE it without crashing — a pattern containing
  `T` must lower to a cir_node tree with `T` marked, deferred.
- This is the deep/broad core (the thing token-level substitution let madc skip).
  Build it incrementally: first just enough for one scalar type param.

**Phase 2 — parse a template pattern to an immutable Tree-1 cir_node.**
- On a template definition, build a `cir_node` PATTERN with dependent params as
  placeholders (no instantiation). Mark it immutable (Tree-1). Start with the
  simplest case (a class template, one type param, scalar members).

**Phase 3 — instantiate by tree-`tsubst` for the simplest case.**
- `tsubst_cir(pattern, args)` = `copy_cir_subtree` + replace placeholder params
  with the concrete arg types in the copied nodes' `datadef`. Feed the resulting
  Tree-2 to c2mir. Select this path for covered patterns; fall back to re-parse
  otherwise. Gated: the covered case runs correctly; everything else unchanged.

**Phase 4 — widen `tsubst` coverage** (non-type params, packs, member templates,
dependent name lookup, partial spec, SFINAE) one construct per commit, shrinking
the fallback. Each commit: a previously-fallback case now goes through tsubst,
gate stays green.

**Phase 5 — delete the re-parse instantiation path** once coverage is complete
(the §5 deletion trigger). `-Wunused-function` confirms the cut.

**Phase 6 — serialize Tree-1 (the header-forest).** Dump the immutable corpus
(Tree-1 + token records + pools); on load, mmap and instantiate against it without
re-parsing the stdlib. This is the "below g++" win and reuses the now-immutable
Tree-1. (Folds in the embedded-header-forest track.)

## 7. FIRST SLICE (start here)

Phase 0 audit + Phase 1 `copy_cir_subtree` proven on a non-template function. That
delivers the safe private-materialization primitive (the thing c2mir's `attr`
mutation requires) with zero behavior change, and is the foundation every later
phase builds on. Concretely:
1. Audit + record the placeholder-param decision here.
2. Implement `copy_cir_subtree`; add a hidden `--xtest-cir-copy` (or a unit test)
   that compiles a deep-copy of a function's body instead of the original.
3. Gate; commit.

## 8. GATE (every commit — correctness only, never perf-gate)
- `make -C src` clean, no new warnings.
- `make -C src fulltest` → 669/0/0/18.
- gcc.c-torture failset byte-identical to the 51-name baseline (0 timeouts).
- `--emit=c11` byte-identical on the representative TUs (vs the prior commit).

## 9. RELATION TO PRIOR WORK
- Builds ON: token-arena 2.2a/2.2b/rec-completion (immutable tokens + slot-ids +
  `cir_node.origin_id`). NONE of that is wasted — it is exactly Tree-1's leaf layer.
- SUPERSEDES: token-arena 2.3 step 2/3 (scratch-token re-parse isolation) — wrong
  layer; g++ doesn't re-parse. Do NOT implement it.
- ABSORBS: §5 PRIMARY lever (materialize-from-AST) AND the embedded-header-forest
  track — they are the same architecture (Tree-1 = the saved/embeddable corpus).

## 10. RISKS
- Biggest: representing & substituting dependent types in a `cir_node` pattern
  (Phase 2/4) is deep and broad. Mitigation: incremental coverage + re-parse
  fallback + per-commit gate.
- c2mir DLIST vs. an indexable cir_node arena: feeding c2mir needs node_t pointer
  form; an arena/index form must fix up to pointers on the way in. Phase 1 can stay
  pointer-form (copy produces DLIST-linked fresh nodes); the index/arena form is a
  serialization concern deferred to Phase 6.
