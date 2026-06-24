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

## 0. RESUME — START HERE (post-compaction; read this section first)

**HEAD:** `396b3c9` on `feature/front-end-performance-claude`. Working tree clean
(only the untracked `mir-debug-support.md` — not ours; leave it). Fork
`/workspace/mir` @ `d3a5cced` on origin/develop; `MIR_COMMIT` = `d3a5cce`.

**✅ PHASE 1.5 DONE (`396b3c9`, 2026-06-23).** `DataDefTemplateParam` (include/datadef.h)
— the typed `T` placeholder: `BaseType::btTemplateParam` (append-only) + `name` +
`param_index`; `is_template_param()` is the single discriminator (DataDefREF/CONST
discipline); every other `is_*()` answers false by construction. `append_type_specs`
guard turns a stray placeholder into an error node (a Phase 2/3 substitution-bug
trap). Purely ADDITIVE — constructed nowhere in production yet (Phase 2 is its first
producer), so emit is byte-identical by construction. GATE GREEN: build clean,
fulltest 669/0/0/18, torture byte-identical (0 timeouts); unit tests `test_datadef`
(type + predicates) + `test_cir` (guard). **NEXT = Phase 2 — DESIGN IS DRAFTED in
§11 (settled direction = hybrid B: concrete shell at parse, tsubst the member
BODIES at cir-build). Before writing code, ANSWER the §11.5 OPEN questions
(chiefly: the 2a one-time dependent-body parse — how to bind a param name to a
`DataDefTemplateParam` without disturbing eager type resolution). Do that with a
fresh budget.**

**✅ PHASE 1 DONE (`c409786`, 2026-06-23).** `CirBuilder::copy_cir_subtree`
(`src/cir_builder.cpp`) + `cir_node.tree1_origin` back-ref + env-gated proof hook
`MADC_XTEST_CIR_COPY` (`src/madc_cir.cpp`). Fork side (pushed): `9d573993` c2mir
O(1) `c2mir_node_first_op`/`next_op`; `d3a5cced` a pre-existing multi-RET `mir.c`
bugfix the baseline depended on (pin gap closed). GATE GREEN: fulltest 669/0/0/18
flag-off AND flag-on; torture failset byte-identical to the 51-name baseline (0
timeouts); `--emit=c11` byte-identical flag-off-vs-on across 12 diverse TUs.
LESSON: a copy must preserve `error_msg` (a BUILD-time madc field, unlike c2mir's
per-compile `attr`) — dropping it silently defeats the `cir_report_errors` gate.
(Phase 1.5 followed — see the block above; current NEXT is Phase 2.)

**SETTLED — do not re-litigate (design owner, 2026-06-23):**
1. Token-arena **2.2a / 2.2b / 2.3-step1 (rec-completion)** are DONE + gated +
   committed (`341dc5a`, `03ba2a8`, `c76e59a`). Token-arena **2.3 step 2/3
   (scratch-token re-parse isolation) is CANCELLED** — wrong layer; g++ doesn't
   re-parse.
2. Adopt the **g++ template model** (VERIFIED against gcc/cp/pt.cc + c2mir — see the
   NOTES doc): two cir_node trees — immutable **Tree-1** (saved patterns + header
   corpus = the forest; the copy source; never to c2mir) and mutable **Tree-2**
   (per-TU, → c2mir; built by `tsubst` = copy + substitute; NEVER re-parse).
3. **c2mir mutates ONLY the node_t base** it understands; `cir_node` extension fields
   (incl. a Tree-1 back-ref, like `origin_id`) are invisible to it ⇒ Tree-2's node_t
   structure must be PRIVATE per instantiation, while the back-ref to immutable Tree-1
   rides safely in the extension. "Some ROM, some RAM."
4. **Pull-based / lazy lexing: CONSIDERED, DEFERRED BY DESIGN — do NOT build it now.**
   It is NOT a g++-parity gap (g++ eager-buffers the whole TU too; clang is the lazy
   one). It is a localized later change — `TokenStream`'s documented "P2-compat" note:
   the parser already consumes via a pull interface (`front()`/`pop_front()`), so only
   the buffer-FILL timing would change (cursor, backtrack, immutable records all
   unchanged). The two-tree architecture (reusable immutable tree + incremental
   mutable tree) is the real IDE foundation AND the parity win — lazy lexing is at most
   a later responsiveness refinement, added only if a concrete IDE scenario demands it.

**NEXT ACTION (first code slice) = Phase 1 `copy_cir_subtree`** (details in §Phase 1 +
the recipe under §"Phase 0 — RESULTS"). It is independent of the dependent-type gap,
zero behavior change, and the foundation every later phase sits on. Steps:
1. Add a `cir_node` extension field for the Tree-1 back-ref (a `uint32` index — the
   tree-level analogue of `origin_id`).
2. Implement `copy_cir_subtree(node)` per the Phase-0 recipe: fresh `uid`
   (`c2mir_next_uid`), `c2mir_init_node_ops`, re-intern strings (`c2mir_uniq_str`),
   copy `code`/`u`/`datadef`/`origin_id`/etc., recurse children over the `base.ops`
   DLIST, append via `c2mir_op_append`; do NOT copy `attr`/`error_msg`; record the
   source-node back-ref.
3. Prove it: compile a deep-copy of a plain (non-template) function's body instead of
   the original — `--emit=c11` and execution byte-identical.
4. GATE (§8): `fulltest` 669/0/0/18 + torture 51-name byte-identical (0 timeouts) +
   `--emit=c11` byte-identical vs the prior commit (build a worktree at the prior HEAD
   for the diff). Commit.
Then Phase 1.5 (template-param placeholder DataDef) → Phase 2 (Tree-1) → Phase 3+
(`tsubst`). Gate every commit; never perf-gate.

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

**Phase 1 — cir_node deep-copy primitive (`tsubst` core, no substitution yet). — DONE 2026-06-23 (`c409786`).**
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

**Phase 1.5 — template-parameter placeholder DataDef (NEW prerequisite; Phase 0 finding). — DONE 2026-06-23 (`396b3c9`).**
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
- **SEE §11 (DESIGN ANALYSIS).** Settled direction = hybrid B (concrete shell at
  parse, tsubst the member BODIES at cir-build). §11.5 lists OPEN questions to
  answer BEFORE coding the 2a dependent-body parse.

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

## 11. PHASE 2 — DESIGN ANALYSIS (recon 2026-06-23; settled vs OPEN)

Recon of the live machinery, written before Phase 2 code so the next session
executes a decision instead of re-deriving it. **Phases 1 + 1.5 are the
foundation this rests on.**

### 11.1 The live machinery (grounded, file:line)
- **`Program::TemplateDef`** (`include/madc.h:1494`): `typeparams` (names, e.g.
  `["T"]`), `body` = a CLONED token vector (`class Name { ... }`), namespace,
  owner, partial-spec pattern/constraint. Registered at template-definition parse.
- **Instantiation = token clone + RE-PARSE, at PARSE time**
  (`Program::instantiate_template_use`, `parser.cpp:3469`): find the `TemplateDef`,
  parse `<args>`, build `subst` (param-name → concrete `TokenDataType`), clone the
  body tokens substituting params, **splice into the token stream and RE-PARSE**
  via the class parser → a concrete `DataDefCLASS` (cached in `datatype_map` by a
  mangled key) + member `TokenFunc`s in `pending_funcs`. Re-parse happens once per
  UNIQUE arg set; it is the ~87%-of-parse cost.
- **cir is built LATER**, post-parse, from those concrete structures
  (`translate_module` → `func_def` → `translate_block`, `cir_builder.cpp:11384`).

### 11.2 THE CORE TENSION (the reason Phase 2 is "the hard part")
g++ has ONE tree (parse == sema == codegen input), so "instantiate at the tree
level" is well-defined. madc SPLITS it: **parse** (tokens → `TokenBase`/`DataDef`,
types resolved eagerly) THEN **cir-build** (post-parse → `cir_node`). But a
concrete type is needed **at parse time**: after `Box<int> b;` the parser must
already know `b`'s members/layout (`b.x` offset) and method signatures (`b.get()`)
to keep parsing. So instantiation **cannot** be fully deferred to cir-build
without breaking parse-time member resolution. The cir-level `tsubst` (Phase 3)
therefore cannot, by itself, replace the parse-time re-parse — something must still
hand the parser a concrete class shell.

### 11.3 Resolution options (with tradeoffs)
- **(A) Full cir-level deferral** — instantiation produces only a dependent
  placeholder at parse time; the real struct + bodies are tsubst'd at cir-build.
  ✗ Breaks parse-time member/layout resolution (11.2). Rejected for the general
  case; only viable for entities never inspected during later parsing.
- **(B) Hybrid: concrete SHELL at parse, tsubst the BODIES at cir-build
  (RECOMMENDED).** Keep producing the concrete `DataDefCLASS` (members/layout/
  signatures) at parse time — cheap relative to bodies — so parsing is unaffected.
  Parse each member-function BODY **once** into an immutable Tree-1 cir pattern
  (params as `DataDefTemplateParam`), and at cir-build instantiate each concrete
  method's body by `tsubst_cir(pattern, args)` (Phase 1 `copy_cir_subtree` +
  placeholder→concrete) INSTEAD of re-parsing the body tokens. Targets the actual
  87% (bodies), preserves correctness, and the re-parse fallback stays for any
  body the pattern path can't yet handle. The shell parse can later be made
  copy-based too (a follow-on), but bodies are where the win is.
- **(C) Two parsed-structure layer** — copy the parsed `TokenFunc`/`DataDefCLASS`
  structure + substitute at PARSE time (no re-parse, no cir change). Plausible but
  duplicates the tsubst machinery at the TokenBase layer and does NOT advance the
  cir_node two-tree (Tree-1 serialization / forest) goal. Deferred.

### 11.4 RECOMMENDED first slice (smallest end-to-end win)
For the simplest case — a class template, one type param, **scalar** members, and
**one member function** whose body uses `T` only in resolvable scalar positions:
1. At template definition, parse each member-function body ONCE with the param
   bound to a `DataDefTemplateParam` (the 2a "dependent parse" — the parser change:
   bind the param NAME to the placeholder DataDef instead of requiring a concrete
   substitution). Produce a per-method **Tree-1 cir pattern** (cir-build that
   dependent body; `append_type_specs` already guards stray placeholders).
2. Keep the concrete shell (members/layout/signatures) on the existing parse path.
3. At cir-build of a concrete instantiation's method, if a Tree-1 pattern exists
   for it, `tsubst_cir(pattern, {T→concrete})` → Tree-2 body instead of lowering
   the re-parsed body. Capability-gated; else fall back.
4. Gate (§8). Coexist with re-parse (deletion trigger = §5).

### 11.5 OPEN questions (resolve with fresh budget, before coding 2a)
- **2a parser change** — exactly where/how to bind a template param NAME to a
  `DataDefTemplateParam` during a one-time dependent body parse, WITHOUT disturbing
  the eager type resolution every non-template parse relies on. This is the deepest
  unknown; scope it first (likely a parse-mode flag scoped to template-body parse).
- **Pattern cache** — key (FuncDef/template identity + member) and where the Tree-1
  cir pattern is stored/owned (CirBuilder arena vs a Program-level pattern cache;
  must outlive per-TU Tree-2 builds).
- **Capability predicate** — "can tsubst handle this pattern?" (drives B's
  fallback). Start: scalar-only `T`, no dependent name lookup, no nested templates.
- **Where cir-build instantiates** — `func_def`'s `translate_block` call is the
  seam (Phase 0), but a per-method pattern needs the concrete method ↔ pattern
  link; confirm how a concrete instantiated `TokenFunc` can find its pattern.

**Status: 11.2–11.4 are the SETTLED direction (hybrid B, bodies-first). 11.5 are
OPEN and must be answered before writing 2a.** Do NOT attempt full cir-level
deferral (A) or the parse-layer copy (C) without re-opening this analysis.

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
