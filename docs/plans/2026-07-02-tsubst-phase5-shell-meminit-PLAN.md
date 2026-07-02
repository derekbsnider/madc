# Phase-5 — shell / ctor mem-init tsubst → DELETE the re-parse path

Status: SCOPED (recon 2026-07-02, grounded file:line). Successor campaign to the
default flip (`a2262c35`, handoff UPDATE-6). Campaign law: `.claude/rules/parse-once.md`.
Parent plan: `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` §11.3(B)
("the shell parse can later be made copy-based too — a follow-on") + Phase 5.

## Why this blocks deletion

The burndown (268 hit / 0 fallback) means no BODY **lowering** uses the re-parsed
tokens. But under hybrid B the instantiation re-parse still RUNS for every
instantiation: it produces (a) the concrete signature/shell the parser needs at
parse time (§11.2 core tension), (b) `FuncDef::ctor_initializers` — mem-init
ARG TOKEN TREES (`include/madc.h:235-239`) parsed by `parse_ctor_initializer_list`,
and (c) the body tokens whose parse product the tsubst hit then discards.
Deleting the re-parse machinery therefore requires the mem-init semantics (b)
to come from Tree-1, and the body parse (c) to be skippable.

## Grounded mechanics (recon 2026-07-02)

- Mem-inits lower in `func_def`'s ctor PROLOGUE (`cir_builder.cpp:15385-15440+`):
  `find_base_initializer` / `find_member_initializer` walk `fd->ctor_initializers`
  and lower `ci->args` (TokenBase trees) via `class_ctor_call_addr` etc. — this is
  INDEPENDENT of `tsubst_method_body` (which replaces only the `{}` body node).
  So today a "hit" ctor takes its body from Tree-1 and its mem-inits from the
  concrete shell's re-parse. (This is how the empty-body pair piecewise/indexed
  ctors are hits: all semantics are shell-side.)
- The PATTERN parse (`build_dependent_pattern` → `parseFunction`) already runs
  `parse_ctor_initializer_list` when the retained decl carries `: inits` — the
  pattern fd gets dependent `ctor_initializers` (args referencing placeholders /
  pack expansions). UNVERIFIED: whether the pattern fd's ctor_initializers
  survive today or are discarded with the pattern parse — check first.
- Pattern granularity: patterns are keyed per concrete-owner member-template
  FuncDef (`fd->dependent_pattern`), so the OWNER (members, bases, layout) is
  CONCRETE inside a pattern. Only the member template's own params are
  placeholders. This makes mem-init pattern-ization tractable: base/member
  resolution in the prologue needs no substitution — only the ARG EXPRESSIONS do.

## Slices (each gated: fulltest + ratchet + burndown flat + sweep; torture when
flag-off-reachable)

1. **Mem-init Tree-1**: at pattern build, lower the pattern fd's ctor_initializers
   into the Tree-1 pattern (pattern-mode arg lowering; prologue-shaped nodes
   attached ahead of the body, or a parallel per-ci node list on the pattern).
   At `tsubst_method_body` hit for a ctor, emit the substituted mem-init nodes
   and have `func_def`'s prologue SKIP its token-side emission for exactly the
   inits the pattern covered (whole-ctor switch, not per-init mixing).
   Start with the pair piecewise/indexed ctors (empty bodies — the pattern IS
   the mem-inits) and `_Rb_tree::_Auto_node`'s ctor.
2. **Body-parse skip**: for a method whose fd has `tsubst_source` +
   `dependent_pattern` and whose instantiation would be a hit, make the
   instantiation parse skip the body tokens (the deferred-body machinery
   already skips bodies at class parse — extend `parse_deferred_lazy_body` /
   the CIR fixpoint to try tsubst BEFORE materializing the lazy body, falling
   back to materialization only when tsubst bails). Measure parse-time win
   (--show-stats): this is the O(n²)-class front-end cost the law cites.
3. **Bail = error**: once slices 1-2 soak green, a tsubst bail on a covered
   shape becomes a LOUD error instead of a fallback (the bail-net's
   swallow-and-reparse behavior dies first).
4. **DELETE**: remove the re-parse instantiation body path + the =0 escape
   hatch + `madc_tsubst_dep_parse_enabled()` + the unit-test fallback-counter
   specimen (test_cir.cpp engagement counters — replace with a hit-counter
   assertion). Build with -Wall; `-Wunused-function` on the deleted web
   confirms the cut (no-parallel-implementations).

## Open questions to answer before slice 1 code

- Does the pattern fd retain parsed `ctor_initializers` after
  `build_dependent_pattern` returns? (The failure path erases funcdef_map
  entries; the success path moves the TokenFunc to fd->dependent_pattern —
  where do the fd-level ctor_initializers live?)
  **Partial recon (2026-07-02):** the pattern FuncDef object survives via
  `pattern->var.type` (only the funcdef_map ENTRY is erased on success,
  parser.cpp:34311). BUT parseFunction accepts `: inits` only when
  `parsing_defaulted_member_template_constructor` is set (parser.cpp:38057-38081
  gate) — set ONLY by parse_deferred_lazy_body's full_definition branch — or
  when the id resolves as `<owner>__<ctor>[__oN]`; the pattern's `__pat<N>`
  rename defeats the ctor-tail resolution. So a ctor pattern parse today most
  likely does NOT take the mem-init path — PROBE how pair's piecewise ctor
  (an empty-body HIT) actually parses its `: pair(...)` span in the pattern
  (does the `:` throw and the pattern still build because...? or is the
  mem-init span stripped before def_tokens?) before designing slice 1.
  ~~First slice-1 step may simply be: set
  `parsing_defaulted_member_template_constructor` (or a pattern-mode
  equivalent) across the pattern parseFunction so the pattern fd's
  ctor_initializers populate.~~
  **PROBED 2026-07-02 — the one-flag version CRASHES; both questions answered:**
  - An env-gated probe at build_dependent_pattern's tail confirmed: the
    mem-init span IS present in a ctor pattern's def_tokens (pair o19:
    `colon_before_body=1 parsed=1`), the pattern parses today ONLY because
    the parser.cpp:38111 skip loop silently DISCARDS the `: inits` span
    (`parse_ctor_initializers` false — the `__pat<N>` rename defeats the
    ctor-tail detection), and `ctor_initializers` is 0 on EVERY pattern fd.
    Also: many never-ODR-used variants' patterns fail to parse (parsed=0 for
    char16_t/char32_t/pmr basic_string ctors) — invisible to the burndown
    because nothing instantiates them.
  - Setting `parsing_defaulted_member_template_constructor = true` across the
    pattern parseFunction (the lazy full-definition precedent) SIGSEGVs
    testmap: unbounded recursion in `TokenCpnd::findVariable` walking a
    compound parent chain (repeated `findVariable+0x168` frames) — the EAGER
    mem-init arg parse runs BEFORE the function's body compound exists and
    re-enters instantiation machinery mid-pattern-parse. The body parse's
    dependent deferral (`dependent_parse_in_progress`) did not prevent it, so
    mem-init args hit an instantiation path that ignores that flag — find it
    before retrying (the backtrace signature is the search key).
  - **Candidate slice-1 design (grounded in existing machinery):** do NOT
    parse mem-inits eagerly in the pattern. Capture the raw span the way the
    class-body sink path already does (`pending_deferred_ctor_inits`,
    parser.cpp:38122+/38139+) onto the pattern fd, then REPLAY it inside the
    pattern's body compound the way `parse_deferred_function_body` replays
    `ctor_init_tokens` ahead of the body (parser.cpp:24162-24173 +
    parse_ctor_initializer_list at 24222) — [class.base.init] complete-class
    context, and the args then parse in the same compound context as the
    body, where the dependent deferral machinery is known to work.
  - **Crash ROOT CAUSE (traced 2026-07-02) — two prerequisite fixes, both
    deepest-layer, needed regardless of eager-vs-replay:** the "recursion" is
    not a compound cycle (only pushCompound assigns TokenCpnd::parent, always
    fresh) — it is UNBOUNDED RE-ENTRY: a delegating mem-init `: pair(...)`
    parses as a construction → `instantiate_member_ctor_template_for_construction`
    → its 34581 `build_dependent_pattern(fd)` for the SAME fd whose pattern is
    mid-build (fd->dependent_pattern still NULL) → parseFunction again →
    same mem-init → ∞. Each cycle pushes compounds, so any findVariable walks
    a thousands-deep parent chain until stack exhaustion (the SIGSEGV site is
    a bystander).
    1. The ctor-construction path lacks the CALL path's dependent deferral:
       mirror parser.cpp:34327 (`dependent_parse_in_progress &&
       args-involve-placeholder` → return, stay dependent, re-resolve at
       copy) in `instantiate_member_ctor_template_for_construction`.
    2. `build_dependent_pattern` needs an fd-keyed in-progress guard
       (re-entrant request returns NULL; the `member_fn_inst_in_progress` /
       mfi_key cycle-break at 34381 is the precedent) — latent today for any
       dependent body that constructs its own class.
    **Both prerequisites LANDED @ `88bef0c3`** (fulltest 674/0/0/16, burndown
    FLAT 268/0; C-unreachable by construction, no torture rerun).
  - **Capture+replay ATTEMPTED and REVERTED (2026-07-02) — the slice has a
    DEEPER blocker, fully probed:** the mechanism itself worked (flag
    `dependent_pattern_ctor_inits` set across the pattern parse; parseFunction
    treats top-level `:` after params as ctor-inits, captures raw via the
    defer path, swaps to an invocation-LOCAL vector immediately after the scan
    loop — the shared `pending_deferred_ctor_inits` leaks across nested
    parses otherwise — and replays inside the body compound with a synthetic
    `{` terminator before parseCompound). BUT source-position probing
    (`[PAT-MEMINIT-CAP] src=` on the first captured token) proved the
    captured spans on NON-ctor patterns are **stray injected-token garbage**:
    to_string's pattern parse captured basic_string's sv-ctor mem-init span
    (`basic_string(__sv_wrapper(_S_to_string_view(__t)), __a)`) with tokens
    stamped basic_string.h:4170 — to_string's `string __str(...)` BODY USE
    SITE, i.e. clone tokens injected by a nested instantiation that ABORTED
    and left its unconsumed decl tokens in the stream. **The pre-existing
    38111 skip loop has been silently EATING this garbage all along** — a
    masked latent bug: any pattern parse that hits a stray `: ...` span
    after its params discards it and "succeeds". Capturing it into
    ctor_initializers is semantic pollution (to_string got a bogus entry), so
    the slice cannot land until the stray-injection hazard is fixed at ITS
    root: find WHICH nested instantiation path injects decl-clone tokens
    (pushToken front-injection) without consuming them on abort, and make it
    restore the stream (the build_dependent_pattern failure path's
    `tokens = saved_tokens` restore is the model). Real-ctor captures looked
    CORRECT (pair indexed ctor `first(...), second(...)`, _Auto_node
    `_M_t(__t), _M_node(...)`) — the mechanism is right, the stream hygiene
    isn't there yet. Probe tooling to reuse: the env-gated
    `MADC_XTEST_PAT_MEMINIT_DEBUG` prints (pattern-tail parsed/ctor_inits +
    capture span/src) — reconstruct from this description; removed pre-revert.
  - **BLOCKER DISSOLVED (2026-07-02, definitive probe) — there are NO stray
    injected tokens.** A skip-loop discard probe + bulk-injection/drain probes
    across all four injector sites (fnbind, pattern, defbody, lazyfull) on
    teststdstringconv showed ZERO stream imbalance (no fnbind drain ever
    fired) and that every discarded `: ...` span belongs to the pattern being
    parsed. The "to_string captured basic_string's mem-init" reading was a
    DOUBLE ILLUSION: (1) `build_dependent_pattern` did not isolate
    `compounds`, so a pattern built mid-body-parse (to_string's body
    `string __str(...)` at basic_string.h:4170 → ctor-construction
    instantiation → pattern build for basic_string's _Tp sv-ctor) saw
    `compounds.top()->method` = the ENCLOSING function and was
    nested-renamed `__ns_std____cxx11_to_string____pat12__17` — reading as
    "to_string's pattern" when it was the SV-CTOR's pattern parsing its OWN
    `: basic_string(__sv_wrapper(...), __a)` mem-init; (2) `clone()` stamps
    tokens with the CURRENT `_parse_file/_parse_line` (TokenBase ctor), so
    decl clones carry the USE-site position — provenance from clone stamps
    is a trap. The capture mechanism was capturing the RIGHT span for the
    RIGHT fd all along. Slice 1 is UNBLOCKED: re-apply the documented
    capture+replay mechanism.
  - **Prerequisite fix LANDED (hermetic pattern COMPOUNDS):**
    build_dependent_pattern now swaps `compounds` out around the pattern
    parseFunction (the instantiate_fn_template_binding model). This kills the
    nested-renaming (patterns keep their `__patN` id, so
    `funcdef_map.erase(parse_id)` now actually erases — previously every
    mid-body pattern build leaked a stale funcdef_map entry under the nested
    name) and stops pattern bodies from wrongly binding enclosing-function
    locals. `class_scope_stack` is deliberately NOT swapped: a first version
    also swapped it and pushed the owner chain, which broke testlocalclassraii
    — with the owner in scope, the pattern's LOCAL class (Guard) registered as
    the concrete `<owner>__<local>` nested class BEFORE the shell parse
    created the real one, hijacking the struct_map entry; the pattern's
    `__patN`-prefixed method TokenFuncs are dropped with the pattern's
    pending_funcs → undefined MIR imports. The local-class naming regimes are
    caller-scope-dependent by construction (parser.cpp:25117 nests ANY class
    declared under a non-empty class_scope_stack — including function-local
    classes; the tsubst local-method remap at cir_builder.cpp:14897 depends
    on the concrete side of that conflation), so patterns must keep
    inheriting the caller's class scope until a principled
    pattern-local-class model exists (future work, not this campaign).
  - **SEPARATE latent bug found by the probe (own track, NOT slice 1):**
    a LOCAL-class ctor hoisted with a nested uid — `__stoa`'s `_Save_errno`
    → id `__ns___gnu_cxx___stoa__o2___Save_errno___Save_errno__1` — defeats
    the parseFunction owner-prefix ctor gate (it strips `__oN` but not the
    nested `__<uid>` suffix), so its REAL mem-init `: _M_errno(errno)` is
    SILENTLY DISCARDED (wrong code: the saved errno is garbage). Affects
    default and =0 modes alike (it is fnbind/local-class machinery, not
    tsubst). The gate's name-surgery detection is the root problem; slice
    1's explicit ctor-ness flag (fd ∈ owner->ctors at pattern/inst build)
    is the right vehicle to replace it.
- Prologue ordering: member inits interleave with base ctors and vptr stores
  in DECLARATION order ([class.base.init]) — the pattern-side nodes must
  reproduce that order or delegate ordering to func_def (prefer: pattern
  carries per-ci arg NODES, func_def keeps ordering/default-init logic and
  consumes substituted arg nodes instead of tokens — smallest change, keeps
  one ordering implementation).
- Delegating ctors (`find_delegating_initializer`) — same treatment, arg
  nodes from pattern.

## Slice-1 state (2026-07-02) — parser half LANDED @ad46a4a3; CIR half design

**Parser half DONE:** ctor patterns capture their `: mem-init` span raw
(consume-once `dependent_pattern_ctor_inits` flag, ctor-ness = fd ∈
owner->ctors, invocation-local span buffer) and REPLAY it inside the body
compound (parse_deferred_function_body model) — SELF-FORGIVING (throw →
drain to pre-replay stream boundary + drop partial inits + isolate poison;
absent ctor_initializers = shell path). Pattern fds now carry dependent
ctor_initializers, verified via the env-gated MADC_XTEST_PAT_MEMINIT_DEBUG
print (kept in-tree at build_dependent_pattern's tail): pair piecewise
delegating=1, pair indexed=2, _Auto_node=2, basic_string sv-ctors=1;
allocators correctly 0. Gates: fulltest 674/0/0/16, burndown FLAT 268/0,
sweep = known 4-noise set.

**CIR half — grounded consumption map (all in func_def's ctor prologue):**
- delegating: `find_delegating_initializer` (cir_builder.cpp ~15398) →
  `class_ctor_call_addr(id("__this"), ocls, ci->args, tf)` — the delegation
  IS the whole prologue ([class.base.init]p6, nothing else runs).
- base: `find_base_initializer` (~15429) → class_ctor_call_addr at the
  base subobject address.
- member: `class_ctor_initializer_stmts(ocls, fd, prologue, tf)` (~15453).
- `explicit_member_inits` name-set (~15413) gates default-member-inits +
  member-construct suppression.

**Design decided:** at pattern build (tsubst_method_body's memoize block,
~14796), ALSO lower the recipe fd's ctor_initializers in pattern mode →
memoized per-source pattern mem-init structure; at hit, copy+subst each ci
arg with the same binding; func_def's prologue takes the WHOLE-CTOR switch
(all-or-nothing: every ci covered by the pattern or none) and keeps
ordering/default-init logic, consuming substituted arg NODES.

**The hard sub-problem:** `class_ctor_call_addr` does per-instantiation ctor
OVERLOAD SELECTION from TOKEN args — substituted NODES can't feed it.
Candidates: (a) relower from origin tokens + substituted types (the
`tsubst_relower_deferred_construction` analog — the machinery the Kind-4
deferred-construction slice extracted); (b) pattern-build-time selection
with a rewritable callee id (the deferred member-call model). Prefer (a):
it already handles pack-expansion args.

**Suite-live targets + their ci shapes:** pair piecewise = DELEGATING ci
(`: pair(__first, __second, _Index_tuple<...>(), _Index_tuple<...>())`);
pair indexed = member inits with NON-TYPE-pack expansions
(`first(std::get<_Indexes1>(__first)...)`); _Auto_node = plain member init
args (`_M_t(__t), _M_node(__t._M_get_node())`). The basic_string sv-ctor
delegating variants are NOT suite-live (never ODR-used). Start with
_Auto_node (plain args) → pair piecewise (delegating, single construction
call = the relower shape) → pair indexed (pack expansion in member-init
args, the hardest).

**CIR half FIRST LANDING (2026-07-02, working tree) — the _Auto_node shape
is LIVE:** `m_tsubst_meminit_patterns[source]` (cir_builder.h) memoizes
per-ci ASSIGN-path arg nodes lowered in pattern mode alongside the body
pattern (admission: no bases/vtable/NSDMIs, NO class-instance members,
every ci a ≤1-arg member init — anything else keeps the shell path via an
absent entry). At hit, the args copy under the SAME binding/pack window as
the body (inside the tsubst_cir block, before the m_tsubst_* restores);
substituted `__this->member = init` stmts (N_ADDR-wrapped for reference
members — the class_ctor_initializer_stmts model) ride at the HEAD of the
returned body block, and `m_tsubst_body_carries_meminits` (cleared at
tsubst_method_body entry, read at func_def's ctor prologue) suppresses the
shell-side `class_ctor_initializer_stmts` — the WHOLE-CTOR switch. A failed
substitution silently keeps the shell path (mem-inits not yet load-bearing
under hybrid B). VERIFIED firing: `[MEMINIT-HIT] fn=..._Auto_node...
stmts=2 failed=0` (env probe MADC_XTEST_PAT_MEMINIT_DEBUG, kept in-tree)
with testmap/testset/testsubscript green. NEXT: pair piecewise (delegating
— needs the whole-prologue-is-the-delegation form + relower for the ctor
call) and pair indexed (pack expansion inside ci args — check whether the
pattern-mode translate_expr of `std::get<_Indexes1>(__first)...` produced a
usable pattern or bailed at admission; probe [MEMINIT-HIT] on testsubscript).

## Slice-1 pair shapes (2026-07-02 cont.) — delegating WIRED, blocked on
## dependent template-id arg types; wall precisely characterized

**Landed (this increment):** the DELEGATING memoize+hit path is wired
end-to-end. Memoize: `find_delegating_initializer` on the pattern fd → a
`delegating=true` entry keeping the ci's TOKEN args (no class-shape
admission — [class.base.init]p6: the target performs the COMPLETE
initialization, so the delegation IS the whole prologue). Hit: relower via
`tsubst_relower_deferred_construction(ci_args, tf, ocls, &binding,
this=id("__this"), yield=false, relax=true, require_overload_match=true)`.
`require_overload_match` is NEW: a delegation names a specific target, so
selection failure must ERROR (clean meminit_failed → shell cis, body stays
HIT) — the blind method_map default-ctor fallback produced a bogus
`Class__Class` call that the emittability gate then (correctly) rejected,
turning previously-HIT pair bodies into FALLBACKs (ratchet red). With the
flag: 35 hit / 0 fallback on testsubscript — ratchet green, honest
"attempted, not yet covered" state. func_def's delegating emission now also
honors the whole-ctor switch (`delegating_ci && !m_tsubst_body_carries_meminits`).
The emittability gate additionally collects callees from meminit_stmts
(delegation/member-ctor calls ride at body head — they need the same
ODR-record + emittable gate). Probes in-tree (env MADC_XTEST_PAT_MEMINIT_DEBUG):
[MEMINIT-MEMO], [MEMINIT-DELEG-FAIL], [RELOWER-SELECT], [TSUBST-UNEMITTABLE].

**The wall (probe-proven):** the delegation's 4 arg datadefs stay DEPENDENT
at hit time — `tuple__Args1`, `tuple__Args2`,
`_Index_tuple___integer_pack_sizeof_____Args1_____` (×2) — so
`select_ctor_overload` scores -1 on every candidate. `subst_datadef` digs
ptr/ref/const/array LAYERS only; a dependent template-id SHELL class
(`tuple<_Args1...>`) has NO structural template-origin metadata (DataDefCLASS
records nothing about "instantiated-from template X with args [...]"), so no
CIR-side substitution can concretize it. The relower's new generic stand-in
(dependent LAYERED args → `tsubst_concrete_arg_token(subst_datadef(...))`
for scoring only) is landed but inert for shells.

**How the shell does it (recon):** `instantiate_fn_template_binding`
(parser.cpp ~32189) is TOKEN-SUBSTITUTION instantiation — it handles
template-id packs (`_Args1` deduced from `tuple<_Args1...>`) and NON-TYPE
tid packs (`_Indexes1`) at 0/1 elements by token surgery, then re-parses the
signature. This machinery SURVIVES hybrid B (signature parse stays per
§11.2) — it is what created the indexed `__o20` instantiations.

**The model to follow:** `resolve_copied_dependent_call` (cir_builder.cpp
~1317) already does hit-time re-resolution WITH instantiation: substituted
arg types → synthetic TokenCallMethod of `tsubst_concrete_arg_token` args →
`instantiate_member_fn_template_for_call` (deduction + instantiate, tid-packs
included). The delegating arm should drive the SAME shape for ctor calls.
Its missing input = concrete arg types:
- `__first`/`__second`: resolvable NOW — pattern ci args that are TokenVar
  references to the recipe's own parameters take their concrete type from
  the INSTANTIATED fd->parameters (the g++ PARM_DECL model: signature params
  substitute once, body references reuse them). Survives slice 2.
- `_Index_tuple` temps: the TRUE wall — `typename
  _Build_index_tuple<sizeof...(_Args1)>::__type()` needs structural
  dependent-type tsubst. Road (i), the principled increment: record
  template-origin metadata (template ref + arg structure, incl.
  sizeof...(pack) / __integer_pack forms) on dependent template-id shells AT
  PATTERN PARSE (the parser has the structure in hand when it creates the
  shell); teach subst_datadef one new case: rebuild via the Program's
  class-template instantiation + member-alias resolution. Serves every KIND
  (Kind-3 rebind included) — this is the parse-once TYPE-half completion.
- Pair INDEXED additionally needs NON-TYPE pack expansion at hit time
  (`std::get<_Indexes1>(...)...` — m_tsubst_active_type_arg_packs is
  type-only today; non-type packs exist only in the parser's
  tid_packs_nontype token substitution).

**Sequencing consequence:** slice 2's body-parse skip can proceed PER-FD for
ctors tsubst fully covers (the _Auto_node class of shapes now); pair joins
once road (i) lands. The mem-init capture+replay (parser half) remains the
transitional carrier for not-yet-covered ctors. Slice 4's DELETE gate
requires road (i) (or an explicitly decided narrower scope) — cost it at
slice-2 time.

**Road (i) recon (2026-07-02, probe [DELEG-ORIGIN] in-tree):** all 4
delegation ci-arg datadefs confirmed `is_dependent_placeholder=1,
has_dependent_surface=0` with clean canonical spellings
(`std::tuple<_Args1>`, `std::_Index_tuple<__integer_pack(sizeof...(_Args1))...>`)
and NO `pending_template_instantiations` record (that registry only covers
the variadic-real-inst shell site, parser.cpp ~3906). THE creation seam is
the opaque-template-args path parser.cpp ~3294-3346: it has `tname`, the
`TemplateDef` (namespace/owner), arg SPELLINGS, and **`raw_arg_tokens`
(per-arg structural token runs)** in hand when it makes the shell — the
structure is simply dropped today. Implementation sketch:
1. Program-side registry `dependent_shell_origin[shell] = {tname, td ref,
   arg spellings, cloned raw_arg_tokens}` recorded at ~3343 (and the ~3906
   variadic site already records args via
   record_pending_template_instantiation — unify or parallel).
2. subst_datadef new case: shell with origin record → substitute each arg
   run under the binding — single template-param name → binding lookup;
   `sizeof...(P)` → pack arity; `__integer_pack(N)...` → 0..N-1 non-type
   expansion (needs PACK CONTEXT threading — subst_datadef is static
   (prog, dd, subst); pack arity lives in the CIR pack window, so either
   thread it or pre-extend the binding with a pack-arity side map).
3. Instantiation seam: the ~3317 partial-spec path shows the parser's
   existing pattern — push cloned `<args>` structural tokens + call
   instantiate_template_use. g++-faithful would build from DataDefs
   directly; check whether a token-free entry point exists before adding
   one (Rule #4). Note the Codex-verification verdict (FINDINGS doc): a
   string re-tokenize resolver was INERT — the structural
   make_typename_type→lookup_member form in subst_datadef is the settled
   direction; arg-token replay into the parser's own instantiation
   interface is structural (cloned type tokens, not source text), distinct
   from the outlawed body re-parse, but weigh it against a DataDef-driven
   entry point at implementation time.
