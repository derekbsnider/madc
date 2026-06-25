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

**HEAD:** `feature/front-end-performance-claude` after the direct type-arg binding,
type-pack metadata capture, direct value/ref/expression/forwarding-call/constructor
pack fan-out, covered system-header placement-new scalar/class `_Up` slices, and the
direct `__destroy(T*)` helper slice, plus local non-pack nested namespace-call tsubst
and the singleton by-value class-object placement-new constructor-pack slice. The only
known unrelated local file during this handoff is untracked
`mir-debug-support.md`; it is not ours.
Fork `/workspace/mir` @ `d3a5cced` on origin/develop; `MIR_COMMIT` = `d3a5cce`.

**✅ 2026-06-24 LOCAL CODEX UPDATE — DIRECT TYPE-ARG BINDING DONE (uncommitted).**
Concrete member-template instances now record parser-resolved TYPE template args in
`FuncDef::tsubst_type_args`, in source `template_param_names` order, beside
`tsubst_source`. `try_instantiate_namespace_fn_template` / `instantiate_fn_template_binding`
fill the vector after explicit args, deduction, and defaults settle; `tsubst_method_body`
binds placeholders from that vector directly, retiring `recover_param_binding`. This is the
g++/clang shape: saved tree plus explicit arg list, not reverse-engineering from the
concrete signature. New coverage pins a body-only template parameter (`T tmp = (T)x`) that
signature recovery could not bind. Conservative fallbacks keep reference-parameter bodies
and dependent nested calls on the existing re-parse path until those constructs are widened.
Follow-up local update also records concrete TYPE parameter-pack elements in
`FuncDef::tsubst_type_arg_packs`, parallel to source `template_param_names`, so CIR pack
expansion will have real pack arity and element types instead of reading token-expanded
reparse output. Later local slices implement direct value/ref/expression/forwarding-call pack call-argument
fan-out (`sink(args...)` from both `Args... args` and `Args&... args`, plus
`sink((args + 1)...)` and `sink(std::forward<Args>(args)...)`): dependent parse captures
`expr...` as `TokenPackExpansion`, including function-call `expr...` forms; the recipe marks
the generated CIR node with pack metadata; `TokenPackExpansion` peels reference/const/pointer
type layers and expression subtrees to find the pack marker/value name; `copy_cir_subtree`
expands marked list children using `tsubst_type_arg_packs`, renames direct value-pack ids such
as `args` to `args__N`, and re-resolves copied dependent callee ids from concrete explicit
template args. The reference-param fallback guard now admits only TYPE-pack reference params,
evaluated against source template metadata for recipe FuncDefs, while system-header template-id
pack bodies stay on parsed-body fallback until broader constructor/destructor pack surfaces are
covered. The next local slice links covered member-template constructors to the same
Tree-1 recipe + parser-owned type/pack argument metadata path, so local constructor bodies
like `Holder(Args... args) { member = sink(args...); }` now fan out by CIR tsubst instead
of relying only on re-parse. The current local slice admits structurally covered
system-header placement-new pack bodies and defers scalar `_Up` lowering until
after type substitution, so allocator-style `new ((void*)p)
_Up(std::forward<Args>(args)...)` can tsubst for scalar/pointer `_Up`. The latest
slice extends that deferred constructed-type path to simple class `_Up` placement
construction when the constructor pack elements are scalar/pointer-like. A later
slice admits singleton by-value class-object constructor packs while multi-element
class-object packs and class-reference packs still fall back. The current slice covers direct
`__destroy(T*)` helpers by emitting a Tree-1 deferred marker for template-parameter
pointees, then lowering class pointees to the concrete destructor and scalar/pointer
pointees to no-op after substitution. It also fixes multi-buffer builtin/intrinsic
registration by reattaching an existing shared `FuncDef` to the active `TokenProgram`,
so split system-header/user parses can capture compiler-intrinsic helper calls during
recipe construction. Validation: `bin/test_cir` 74 test cases / 909 assertions / 4 skipped,
`make -C src fulltest` 669/0/0/18, and `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18. The latest local slice extends copied dependent-call re-resolution to local
non-pack namespace function-template calls nested inside covered member-template bodies,
such as `sink(nn::ident(v))`: the copy path substitutes the argument types and reuses the
normal namespace overload resolver for the callee id, while non-pack system-header
dependent calls deliberately mark the copy unsupported so the parsed-body fallback stays
active. Validation after that slice: `bin/test_cir` 75 test cases / 921 assertions /
4 skipped, `make -C src fulltest` 669/0/0/18, and `MADC_XTEST_DEP_PARSE=1 bash
scripts/run_tests.sh` 669/0/0/18. The latest local slice covers singleton by-value
class-object constructor packs in allocator-style placement-new bodies, keeping the
pack expression marked until copy-time substitution so the class object argument is
renamed and re-resolved inside the instantiated body; validation after that slice:
`bin/test_cir` 76 test cases / 935 assertions / 4 skipped, `make -C src fulltest`
669/0/0/18, and `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 669/0/0/18.
**Current next blocker: broader CIR pack expansion for real system-header
forwarding/destructor pack patterns, multi-element/class-reference placement-new
constructor argument packs, broader system-header nested/dependent calls, and
template-id body/return surfaces; Phase 5 delete-reparse remains gated on full
coverage.**

**✅ PHASE 3 FIRST SLICE + MARKER WIDENING DONE (2026-06-24) — the recipe is CONSUMED by
tsubst.** Four gated commits: `e4dda75` (tsubst_cir core), `6c301f9` (FuncDef::tsubst_source
instance→source link), `2bf8696` (the func_def seam: a covered method's BODY built by tsubst
of the memoized Tree-1 recipe pattern), `2459a7e` (deferred type-spec MARKER so a body using
`T` as a TYPE can tsubst — expansion unit-tested). g++'s instantiate_body / TEMPLATE_TYPE_PARM
shape on our arena. Validated firing + byte-identical (Holder::set, testoutoflinemembertemplate
::store). Behind MADC_XTEST_DEP_PARSE (prod byte-identical). Gate green flag-off AND flag-on
669/0/0/18 every commit. The dependent-PARSE-of-`T`-as-a-type gap was later
resolved in the handoff §0 local update; the direct value/ref/expression-pack fan-out slices
and first forwarding-call pack fan-out slice are also local, and the current blocker is the
broader system-header/dependent-call pack/template-id expansion set.
**FULL detail + the fix + reducer in the handoff §0:**
`docs/plans/2026-06-24-two-tree-phase3-tsubst-consume-HANDOFF.md`.

**✅ PHASE 2 — COMMIT 1 DONE (`b35cef2`, 2026-06-24): the scoped template-param
registry.** The 2a foundation (PLAN §11.5a): `Program::template_param_scopes` (a
stack of {param-name → `DataDefTemplateParam*`} frames) + RAII `TemplateParamScope`
guard (twin of `NamespaceScope`) + `intern_template_param` (pooled by (name,index),
owned by `template_param_pool` for Program lifetime, ptr_type_cache convention) +
`resolve_template_param` (innermost-first; O(1) when no frame). `resolve_current_class_type_alias`
consults the registry FIRST — single-point injection covering all 7 callers, so a
param `T` is visible as a type everywhere a class-scope type name already resolves,
with NO datatype_map change and NO resolver rewrite. Purely ADDITIVE + INERT
(`TemplateParamScope` constructed nowhere in src/ — only the unit test; production
stack always empty → `resolve_template_param` always NULL → consult branch never
taken → emit byte-identical by construction, the Phase 1.5 class). GATE GREEN: build
clean; fulltest 669/0/0/18 + drift gates; torture byte-identical to 51-name baseline
(33c+18r, 0 timeouts); --emit=c11 clean across diverse TUs. **✅ PHASE 2 — PRODUCER + DEFER GUARD DONE (`2104299`, 2026-06-24).**
`build_dependent_pattern` parses a member-fn-template body ONCE with its param →
`DataDefTemplateParam` placeholder, captures the TokenFunc as
`FuncDef::dependent_pattern` (a Tree-1 RECIPE), removes it from `pending_funcs`.
Capability-gated by `tsubst_eligible` (one type param, no pack, non-dependent return,
no template-id/`T::`). THE KEY PIECE = a `dependent_parse_in_progress` guard: the
instantiation entry points bail while a dependent body is parsing, so a nested call
stays DEPENDENT (bound to its body-less placeholder) instead of eagerly instantiating
with the param placeholder as a concrete arg (the leak an unguarded parse caused:
657/12). Producer runs only behind env hook `MADC_XTEST_DEP_PARSE`; recipe consumed by
nobody yet. GATE GREEN: build clean; fulltest flag-OFF **AND flag-ON 669/0/0/18**
(full isolation); torture byte-identical (33c+18r, 0 timeouts); --emit=c11 clean.
**✅ §11.5c WIDENING STEP 1 DONE (`20bbf92`, 2026-06-24): the per-call dependence test.**
`call_involves_placeholder` / `datadef_involves_placeholder` (parser.cpp) replaced the
global bail at both instantiation entry points — they now defer ONLY genuinely
type-dependent calls (g++'s `any_type_dependent_arguments_p`, pt.cc:30555), so a
non-dependent op (`cout<<"x"`) inside a dependent body instantiates eagerly as g++ does.
`dependent_parse_in_progress` is now purely the "in a dependent parse" gate. GATE GREEN:
fulltest flag-OFF AND flag-ON 669/0/0/18; torture/emit byte-identical (production path
unreachable-changed). Soundness gated now; precision validated at Phase 3.
**NEXT = PHASE 3 — CONSUME the recipe (tsubst). FULL imperative handoff:
`docs/plans/2026-06-24-two-tree-phase3-tsubst-consume-HANDOFF.md`.** In short: cir-build
the recipe (FuncDef::dependent_pattern) → Tree-1 cir pattern (fuse build+substitute so a
placeholder never reaches `append_type_specs`); `tsubst_cir(pattern,{T→concrete})` reusing
`copy_cir_subtree` (cir_builder.cpp:379) + placeholder→concrete (incl ptr/ref); wire at the
`func_def`→`translate_block` seam (cir_builder.cpp:11458) for covered methods + re-parse
fallback; method↔pattern link via the instantiation path. CRUX RISK: first slice where
output changes → `--emit=c11` byte-identical means tsubst output == re-parse output for
covered methods — go SCALAR-FIRST. Remaining §11.5c widening (typeless placeholder + ADL
bit; real `is_type_dependent` + tsubst re-resolution) comes AFTER scalar consumption.**

**✅ PHASE 1.5 DONE (`396b3c9`, 2026-06-23).** `DataDefTemplateParam` (include/datadef.h)
— the typed `T` placeholder: `BaseType::btTemplateParam` (append-only) + `name` +
`param_index`; `is_template_param()` is the single discriminator (DataDefREF/CONST
discipline); every other `is_*()` answers false by construction. `append_type_specs`
guard turns a stray placeholder into an error node (a Phase 2/3 substitution-bug
trap). Purely ADDITIVE — constructed nowhere in production yet (Phase 2 is its first
producer), so emit is byte-identical by construction. GATE GREEN: build clean,
fulltest 669/0/0/18, torture byte-identical (0 timeouts); unit tests `test_datadef`
(type + predicates) + `test_cir` (guard). **NEXT = Phase 2 — DESIGN DRAFTED in §11.
Settled direction = hybrid B (concrete shell at parse, tsubst member BODIES at
cir-build). The 2a intercept is SETTLED AND its mechanism is VERIFIED against existing
infrastructure (§11.5a): parse-time scoped type-name resolution already exists —
`resolve_current_class_type_alias` (parser.cpp:2035) over a push/pop'd
`class_scope_stack`, consulted BEFORE `datatype_map`, and already pushed during
template body parse (madc.h:1737). 2a = resolve the active template's params to
their `DataDefTemplateParam` through THAT path (no resolver rewrite, no datatype_map
change). FIRST STEP = code 2a (the one-time dependent body parse + param→placeholder
via the scoped path), build the Tree-1 body pattern, then Phase 3 tsubst. §11.5b
lists the remaining OPEN items (pattern cache, capability predicate, method↔pattern
link) — answer while coding, not before.**

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

### 11.5a 2a INTERCEPT — recon answer (was the deepest unknown; now SETTLED mechanism)
Recon (2026-06-23) of the type-name gate downgraded this from "deepest unknown" to
a concrete, low-invasiveness plan:
- **Type-ness is gated at PARSE time through `datatype_map.find(name)`** (+ `struct_map`,
  `lazy_resolve_type`, `resolve_current_class_type_alias`) — e.g.
  `resolve_expression_class_scope` (`parser.cpp:2094`) and the declaration-parse
  type checks. A bare identifier becomes a type (and `T value;` becomes a member
  DECL rather than an expression) iff one of these resolves it.
- **A template body is stored as CLONED tokens** captured at definition (`parser.cpp`
  ~4362/4529/4544), where `T` is a `TokenIdent` (T was not a type yet). Today
  instantiation REPLACES that `TokenIdent` with a concrete `TokenDataType` before
  re-parsing. The 2a dependent parse instead does NOT substitute.
- **SETTLED mechanism:** for the one-time dependent body parse, register each
  template param name as a `DataDefTemplateParam` in a **scoped** type registry
  (push before the body parse, pop after) so the EXISTING `datatype_map.find` path
  resolves `T` to the placeholder with **NO resolver rewrite** — precisely "without
  disturbing eager type resolution" (the lookup code is unchanged; only the scoped
  registry content differs during the dependent parse). madc already carries
  scoped template parse-mode state to model this scope:
  `instantiating_canonical_spelling`, `fn_template_instantiation_depth`, the
  `compounds` stack, `class_scope_stack`, `parsing_template_instantiated_member_body()`
  (`parser.cpp:7421`).
- **VERIFIED — the scoped parse-time path already exists (existence proof, recon
  2026-06-23):** parse-time, scoped type-name resolution is not hypothetical —
  `resolve_current_class_type_alias` (`parser.cpp:2035`) walks a **push/pop'd
  `class_scope_stack`** (`madc.h:1897`) via `resolve_class_type_alias` and is
  consulted FIRST in `resolve_expression_class_scope` (`parser.cpp:2088`), BEFORE
  the global `datatype_map`. So a name can be made to resolve to a type for the
  duration of a scope and then vanish — exactly the 2a requirement. And the scope
  is ALREADY pushed during template body parse: `FnTemplateDef.owner_class`
  (`madc.h:1737`) "pushed on class_scope_stack during the body parse so the
  params/body resolve." (`typedef` is the simpler proof that a mid-parse-registered
  name is promoted to a type at its use site.)
- **2a wiring (concrete):** attach the active template's params to this existing
  scoped resolution — either resolve a param name to its `DataDefTemplateParam`
  inside `resolve_current_class_type_alias`/`resolve_class_type_alias` when an
  active-template-params scope is on the stack, OR push a sibling scoped
  param-name→placeholder registry consulted alongside `class_scope_stack`. NO new
  resolver branch in the general type lookup, NO change to `datatype_map`
  resolution. The residual TokenIdent→type-promotion question is ANSWERED by these
  existence proofs (class-scoped aliases + typedef both promote a mid-parse name to
  a type at parse time); no separate probe needed.

### 11.5b — RESOLVED (recon 2026-06-24, grounded in file:line; commit-1 registry landed)
The scoped param path (§11.5a) is BUILT (`b35cef2`). The three remaining items are now
SETTLED — execute, do not re-derive:

- **THE ARCHITECTURAL SPLIT (decides everything below): the dependent body parse runs
  at PARSE time; the Tree-1 *cir* pattern is built at CIR-BUILD time.** `translate_block`
  (cir_builder.cpp:11458, inside `func_def`) runs ONLY from `translate_module` — never
  during the definition parse. So a Tree-1 pattern exists in TWO stages:
  (1) a dependent PARSE-TREE pattern (`TokenFunc`/`TokenCpnd` whose `T` resolved to a
  `DataDefTemplateParam` via a pushed `TemplateParamScope`), produced ONCE at parse time
  and stored; (2) the dependent parse-tree is cir-built ONCE into the immutable Tree-1
  cir pattern at cir-build time; (3) Phase 3 `tsubst_cir` copy+substitutes that cir
  pattern per concrete instantiation at the 11458 seam. The dependent parse MUST run in
  full isolation — the `instantiate_template_use` save/restore model (parser.cpp:4178+:
  swap `compounds`/`class_scope_stack`/`cur_func_name`, RAII restore, exception-safe) —
  so producing a pattern has no side effects on the existing path.

- **Pattern cache (was: key + ownership).** REUSE existing FuncDef provenance, do NOT
  invent a parallel store. Member-fn templates already carry `member_template_decl`
  (raw tokens) + `member_template_owner` + `template_param_names`/`_is_pack`/`_is_type`
  (madc.h:175-196): attach the dependent parse-tree pattern + a cir-pattern slot HERE on
  the FuncDef (Program lifetime). Class templates: a per-(TemplateDef, member) cache
  keyed by the member's identity. The CIR pattern itself lives in the CirBuilder Tree-1
  arena — built once, never freed mid-compile, shared across per-TU Tree-2 builds (it IS
  immutable Tree-1). The FuncDef pointer itself is the natural key for member-fn templates.

- **Capability predicate (`tsubst_eligible`).** Conservative; false → existing re-parse
  fallback (no behavior change). TRUE only for: exactly one template param, all type-
  params (`template_param_is_type` all true), no packs (`template_param_is_pack` all
  false), and a body using `T` only in scalar positions — NO dependent name lookup
  (`T::x`), NO nested template-id (`Foo<T>`), NO `sizeof...`. A token-scan of the body
  rejects the disqualifying constructs. Widen one construct per Phase-4 commit.

- **Method↔pattern link.** Member-fn templates: the instantiated FuncDef is produced
  from `member_template_decl` — record the SOURCE template FuncDef (identity) on the
  instantiated FuncDef so the 11458 seam finds the pattern + the concrete type args.
  Class templates: the instantiated `DataDefCLASS` records its `TemplateDef` identity +
  arg list (mangled key already exists); each member maps by name/index to its pattern.

### 11.5c — the side-parse producer + the DEPENDENT-CONTEXT defer (RESOLVED for the scalar slice; widening path is g++-grounded)
`build_dependent_pattern` parse-captures a member-template body ONCE (modeled on
`parse_deferred_lazy_body`: snapshot `pending_funcs`/parse state, push body tokens, push
`TemplateParamScope`, `parseFunction` with a throwaway name, capture `pending_funcs.back()`,
pop it off, erase the throwaway `funcdef_map` entry). The Tree-1 RECIPE is stored on
`FuncDef::dependent_pattern`.

**Blocker hit + FIXED.** An UNGUARDED dependent parse went flag-ON 669/0 → 657/12 with
`unsubstituted template parameter '_Up' reached type lowering`: parsing a body in madc
EAGERLY INSTANTIATES the templates it uses (nested `construct`/operator/`_M_realloc_insert`
calls), binding the param PLACEHOLDER as a concrete arg and registering under THEIR names
(leaking past the throwaway cleanup). **FIX = a `dependent_parse_in_progress` suppression
guard**: set across the dependent parse; the instantiation entry points
(`instantiate_member_fn_template_for_call`, `try_instantiate_namespace_fn_template`) bail
when it is set, so a nested call stays bound to its body-less placeholder (dependent)
instead of instantiating. **Flag-ON the FULL suite is now 669/0/0/18** — complete isolation;
the 3 previously-leaking tests pass. Producer + guard are committed; flag-OFF is
byte-identical (the guard branch is never taken, `build_dependent_pattern` is uncalled).

**RECON (gcc + clang, 2026-06-24) — the global flag is a SCALAR-ONLY crutch; here is the
correct primitive.** g++ has NO "defer mode": dependence is computed PER-NODE from operand
types (`type_dependent_expression_p`, pt.cc:30086); `processing_template_decl` is only a
gate ("in a template at all?"). A dependent call becomes a TYPELESS placeholder node
(`build_min_nt_call_vec`, semantics.cc:3370) — no overload res, no instantiation; an ADL-
wanted bit is stashed (`KOENIG_LOOKUP_P`). `tsubst_expr` CALL_EXPR (pt.cc:22158) later re-
runs the SAME `finish_call_expr` (pt.cc:22515) on now-concrete args — one resolver, two
phases. Clang names the breaking case: in `add(T x,int y)`, `x+y` is dependent but `y` is
NOT — a non-dependent sub-expression inside a dependent body must be bound/instantiated
EAGERLY at definition (two-phase lookup). **Our global bail-ALL flag suppresses those too**
→ it is sound ONLY for bodies with no non-dependent instantiation-triggering op (scalar T,
no `cout<<"x"`, no fixed-type helper/`vec.size()`). That is "essentially every real STL
member body" the moment we widen. **Why it's safe NOW: the recipe is INERT** (consumed by
nobody) — a mis-deferred non-dependent call is thrown away; the real compile uses the
untouched re-parse path. The time bomb detonates only when **Phase 3 CONSUMES** the recipe.

**WIDENING PATH (do BEFORE Phase 3 consumes recipes from non-trivial bodies):**
1. Replace the global gate with a PER-CALL dependence test — bail only if the call's
   callee/args actually involve the placeholder type (the `any_type_dependent_arguments_p`
   analogue, pt.cc:30555); keep `dependent_parse_in_progress` purely as the "in a dependent
   body" gate. Same code shape, one loop. Fixes the `cout<<"literal"` / non-dependent-call
   class.
2. Make a deferred call a TYPELESS placeholder (madc analogue of `unknown_type_node`) +
   record an ADL-wanted bit, instead of binding to a body-less concrete decl.
3. Build a real `is_type_dependent(expr)` predicate from operand types; at tsubst, re-run
   the NORMAL call-resolution entry point on concrete args (reuse, don't fork) — retires
   the flag. This IS §4's "representing dependent constructs," now g++-grounded.

**Status: 11.2–11.4 SETTLED (hybrid B); 11.5a/11.5b SETTLED + grounded; 11.5c = producer +
scalar-slice defer DONE & gated, widening path g++-grounded.** Commit-1 (registry) + the
producer (capability-gated, inert, suppression-guarded) are the foundation. NEXT real work =
the per-call dependence test (widening step 1) THEN cir-build the recipe to a Tree-1 cir
pattern THEN Phase 3 tsubst at cir_builder.cpp:11458. Re-use the env-gated flag-on
byte-identical harness as the validator. Do NOT attempt full cir-level deferral (A) or the
parse-layer copy (C) without re-opening this analysis.

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
