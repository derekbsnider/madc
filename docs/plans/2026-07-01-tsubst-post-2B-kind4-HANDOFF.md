# tsubst post-2B + Kind-4 — HANDOFF for fresh context (2026-07-01, evening)

> ## 🚩 UPDATE 6 (2026-07-02, @ `a2262c35`) — **THE DEFAULT FLIP IS DONE: parse-once tsubst is the production path**
>
> All flip blockers cleared in three commits, each fully gated (fulltest 674/0/0/16
> exit 0, burndown FLAT 268/0, ratchet GREEN, REAL torture byte-identical ×2):
> - **`b94ae108` — compound balance on THROW** (flip blocker 1, the 4 flag-on-only
>   failures): a throwing dependent-pattern parseFunction leaked its pushed
>   compounds (build_dependent_pattern's catch swallows); every later lazy body
>   parsed as NESTED (`<encl>__<name>__<uid>`) orphaning plain-symbol call sites
>   (`_M_construct__o3` undefined import in testfstream/teststdstringconv; the two
>   SIGSEGVs testifconstexpr/testinvocable were already fixed by the 100% slice).
>   Fix: CompoundBalanceGuard RAII at parseFunction entry (unwind via popCompound
>   during exception unwinding only). The 4 tests joined the flag-on ratchet.
> - **`8b296e7e` — local-class TAG REBIND** (flip blocker 2, the warning diffs):
>   a pattern-local class with CONCRETE members (_M_construct's _Guard) escapes
>   the dependent-member marker, so type_list baked the PATTERN's tag string into
>   Tree-1; the 14864 remap bound the class but the tag N_ID string was copied
>   verbatim → c2mir pointer-warning pair on ~62 tests. Fix: stamp tag-ref
>   datadef in pattern mode (type_list + class_tag_ref) + rename the copied tag
>   N_ID when the binding remaps it. Sweep 68 → 4, and ALL 4 survivors are proven
>   run-to-run nondeterminism (ASLR backtraces ×3 + teststructinit indeterminate
>   print) — ZERO real flag-on/off diffs remain.
> - **`a2262c35` — THE FLIP**: `madc_tsubst_dep_parse_enabled()` (datadef.h /
>   madc_globals.cpp) — default ON, `MADC_XTEST_DEP_PARSE=0` = soak escape hatch
>   (dies with the re-parse machinery). parse-once.md rule+docs updated in-commit.
>
> **Remaining to the parse-once end-state (the deletion):** Phase-5 — the
> concrete SHELL (signature + ctor mem-inits) still parses via the re-parse
> machinery under hybrid B; mem-inits must come from a Tree-1 pattern before the
> body/decl re-parse machinery can be DELETED. Then delete (`-Wunused-function`
> confirms the cut) and the unit-test fallback-counter specimen dies with it.
> Plan anchor: `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` §11.3(B)
> "shell parse can later be made copy-based too (a follow-on)" + Phase 5 (§ list).

> ## 🏁 UPDATE 5 (2026-07-02, @ `297fa08e`) — **100% BURNDOWN: 268 hit / 0 fallback**
>
> `scripts/tsubst_burndown.sh`: "re-parse is unreachable across the suite;
> delete-the-path gate is MET". The whole `_Rb_tree` trio is verified runtime hits;
> every flag-on gate test is fully parse-once (set 10/0, map 11/0, vector 15/0,
> subscript 35/0, containerdtor 34/0, madc_ns 34/0, localclassraii 1/0, + the two
> scalar-ref-param reducers 1/0). Session arc: 66% → 91% → 95% → 96% → 97% → **100%**.
>
> The 100% slice (see the `297fa08e` commit message for full mechanics): dependent
> functor-call parse (dependent operator() member call + arity-unknown bypass), CIR
> unknown-receiver member-call deferral (rewritable callee id), dependent-call-swallow
> POISON (`dependent_parse_poisoned`), concrete-template-id carve-out (subsumes the
> cast-only one), and the owner-first member-typedef lookup reorder ([class.mfct] —
> flag-off-reachable, validated by fulltest + a REAL torture run, failset
> byte-identical). A class_scope_stack swap+push was tried and REVERTED (local-class
> enclosure collision — testlocalclassraii regression caught by the gate).
>
> ### What burndown=0 does NOT mean — the remaining track to the default flip
> (`.claude/rules/parse-once.md`: 0 fallback → flip the default → delete the re-parse
> path). Blockers for the FLIP, tracked separately:
> 1. **4 pre-existing flag-on-only failures** (tmp/flagon_diff_sweep.sh finds them):
>    testifconstexpr + testinvocable SIGSEGV rc=139, testfstream + teststdstringconv
>    rc=1.
> 2. The 68 cosmetic flag-on warning diffs (incl. the local-class ctor `this`-capture
>    warning pair — see UPDATE-3's latent re-parse `static`-drop note for why some are
>    re-parse bugs the tsubst path FIXES).
> 3. Phase-5 ctor mem-init tsubst (mem-inits ride the shell today) — needed before the
>    re-parse path can be DELETED (the shell parse still uses it).
> 4. The unit-test fallback specimen (test_cir.cpp engagement counters) uses
>    `typename T::type` (Kind-3 `dependent name P:: in body`) — the last reject class
>    with zero suite occurrences; it dies when Kind-3 lands for the flip.

> ## ✅ UPDATE 2026-07-02 — §2 lever 1 LANDED @ `44febff9` (empty-body multi-pack slice)
>
> **Burndown 243/23 (91%) → 255/11 (95%).** The pair piecewise/indexed ctor family
> (`>1 pack param` 6 + `non-type template param` 6) is DONE — both are tsubst HITS;
> testmap (11/0) and testsubscript (35/0) are FULLY parse-once flag-on. The fix was
> MUCH smaller than §2's "multi-pack fan-out state" estimate because **both ctors have
> EMPTY `{}` bodies** — all semantics live in the mem-initializer list, which rides the
> SHELL under hybrid B (the same model as 2B's basic_string range-ctor hit):
> - `tsubst_eligible` hybrid-B region split (parser.cpp): the body block is located by
>   backward brace-match from the decl's final `}`; a SHELL-side template-id (params +
>   ctor mem-inits, `i < body_open`) no longer disqualifies; an EMPTY body admits
>   multiple packs and NON-TYPE packs (nothing to substitute). Non-type SCALARS and
>   non-empty multi-pack bodies still reject.
> - `collect_ordered_type_arg_bindings` (parser.cpp): non-type pack VALUES
>   (`tid_packs_nontype` int64s — deduction already existed) now fill
>   `tsubst_type_arg_packs` as value-DataDefs (decimal-name representation,
>   `datadef_is_nontype_constant`). No CIR-side change needed (fan-out never runs on
>   an empty body).
> - **Probe intel (env-gated superset experiment, run before designing):** the 9
>   remaining `template-id '<' in body` fallbacks yield **0 hits** under full
>   gate-skip — they late-bail as ref-param(6)/recipe(3). Gate-widening is worthless
>   for them; they need real Kind-3 dependent-member-type resolution (§2 lever 2).
> - **New rigor tool:** `tmp/flagon_diff_sweep.sh` — diffs EVERY test's output flag-on
>   vs flag-off. HEAD baseline: 68 pre-existing warning-diff tests + **4 PRE-EXISTING
>   flag-on-ONLY failures** (testifconstexpr/testinvocable SIGSEGV rc=139,
>   testfstream/teststdstringconv rc=1) — these predate this slice, are a separate
>   track, and BLOCK an eventual flag-default flip.
> - Gates: fulltest 674/0/0/16 exit 0; flag-on baseline bumped in-commit (map 11/0,
>   set 7/3, vector 15/0, subscript 35/0, containerdtor 30/3, madc_ns 30/3); sweep
>   byte-identical to HEAD baseline; torture failset byte-identical (memcpy-a2.c
>   suite-load timeout, verified PASS isolated).
>
> **Remaining [why:] worklist: `template-id '<' in body` 9 (Kind-3),
> `reference-param value-read` 1, `recipe parse failed` 1.** §2's remaining levers:
> Kind-3 (lever 2), the local-class ctor `this`-capture bug (lever 3, now joined by
> the 4 sweep-surfaced flag-on-only failures), mop-up (lever 4).

> ## ✅ UPDATE 3 (2026-07-02) — wall-map item 4 LANDED: scalar-ref-param slice — **257/9 (96%)**
>
> `reference-param value-read` class GONE (×2 → hits). ONE reason-class remains
> suite-wide: `template-id '<' in body` ×9 (the `_Rb_tree` trio — wall-map items 1–3).
> Two root causes, both fixed at the deepest layer, no guard/narrowing kept:
> - **The blanket guard was stale**: a ref-param value read lowers to an UNTYPED
>   `N_DEREF(id)` that c2mir types from the concrete instantiated SHELL's parameter
>   declaration — per-instantiation re-typing for free (g++ oracle: pt.cc
>   `tsubst_copy_and_build` INDIRECT_REF rebuilds via `build_x_indirect_ref` on the
>   substituted operand). `tsubst_body_has_unsupported_reference_param` +
>   `tsubst_reference_param_is_type_pack` DELETED outright.
> - **The one real hazard the guard masked**: `is_size1_pointer` classified a
>   DEPENDENT pointee as `void*` (DataDefTemplateParam's placeholder repr rawtype is
>   dtVOID), baking the GNU size-1 `(char*)` rewrite into the shared pattern —
>   `base + a` became pointer arithmetic (right value, wrong type, c2mir warning).
>   Fix: a dependent pointee is "not known yet", NOT void (`template_param_under_
>   type_layers` check in the classifier). Latent edge documented: an instantiation
>   binding Arg=void* that does arithmetic would now need copy-time re-decision
>   (loud c2mir error, not silent wrong code; no suite shape does this).
> - **Probe methodology paid off again**: the emitted-C diff also showed the
>   RE-PARSE path silently DROPS `static` on a body local (`static Node n; return
>   &n;` → dangling pointer flag-off) while the tsubst body preserves it — the
>   dying path had the bug, the new path is gcc-faithful.
> - Unit-test specimen updated: the engagement-counters test's fallback example was
>   the deleted guard's `T&` shape (now a HIT, `got==47` still exact through the
>   tsubst body); replaced with a Kind-3 `typename T::type` body (verified on
>   bin/madc: 1 hit / 1 fallback, `dependent name P:: in body`).
> - Flag-on gate list grew: + testfunctortmploperator, testmembertmplptrret (1/0
>   baselines). Gates: fulltest 674/0/0/16 exit 0; burndown 257/9; sweep identical
>   to baseline (the two reducers' DIFF entries gone); torture byte-identical by
>   construction for C (the classifier change only alters behavior when a
>   DataDefTemplateParam is present — impossible in C code; C++ both-flag coverage
>   via fulltest + gate).
>
> **NEXT: wall-map items 1–3 in order** — (1) `_Alloc_node::operator()` forward
> carve-out, (2) `_M_insert_` dependent functor-call parser fix (⚠ the assignment-
> garbage hazard), (3) `_M_insert_unique` return-construction deferral.

> ## ✅ UPDATE 4 (2026-07-02) — wall-map item 1 LANDED: forward carve-out — **260/6 (97%)**
>
> `_Rb_tree::_Alloc_node::operator()<_Arg>` ×3 are HITS. Remaining 6 = `_M_insert_` ×3
> (`dependent functor call in body` — its true blocker, now named) + `_M_insert_unique`
> ×3 (`template-id '<' in body`). BOTH need wall-map item 2 (the dependent functor-call
> parse) as the keystone. Three pieces landed:
> - **Eligibility carve-out** (`tsubst_lt_opens_call_template_id`, parser.cpp): an
>   explicit-template-argument CALL (`forward<_Arg>(x)` — identifier callee, `<` span of
>   plain identifier args, followed by the call's `(`) no longer disqualifies a body —
>   the Kind-1 copy machinery re-resolves it per instantiation.
> - **The landmine got a fence**: the first probe admitted `_M_insert_` too (same forward
>   shape inside it) → testset FAILED flag-on (rc=1) — the functor call on a placeholder-
>   typed param mis-parses NON-self-detectingly, exactly as the wall map warned. New
>   eligibility clause: a body CALL through a value parameter whose declared type
>   involves a template param (`dep_value_params` from the shell param list; `.`/`->`/
>   `::`-qualified uses exempt) rejects as `dependent functor call in body` until item 2
>   lands. NEVER admit `_M_insert_` before that parser fix.
> - **Identity-refcast builtin kind "forward"** (shape-detected at header retention,
>   Rule-#7-clean: single param + body `{ return static_cast<...&/&&>(param); }` last-
>   statement — std::forward/std::move; g++ analogue: -ffold-simple-inlines "magic"
>   fns). Consumer: `object_arg_addr`'s pattern-mode deferred-arg marker unwraps the
>   identity call and defers on the INNER variable (safe even for non-identity
>   substitutions — the copy re-lowering's offset-0 layout guard self-detects).
> - Gates: fulltest 674/0/0/16 exit 0 (unit 21/21); gate PROGRESS set 8/2,
>   containerdtor 31/2, madc_ns 31/2 (baseline bumped in-commit); burndown 260/6;
>   sweep 68 diffs, set identical to the scalar-ref-param baseline; torture
>   byte-identical by construction (eligibility behind env-gated
>   build_dependent_pattern; marker behind pattern mode; "forward" kind has no
>   flag-off consumer and no C-code reachability).

> ## 🗺 UPDATE 2 (2026-07-02, @ `9873d7dd`) — THE ROAD TO 100%: complete wall map of the last 11
>
> All 11 remaining fallbacks are IDENTIFIED (per-test scanner: `tmp/tsubst_fallback_scan.sh`).
> Landed this pass: (a) **operator-id pattern-parse fix** in `build_dependent_pattern`
> (multi-token `operator()` name — its own `(` was mistaken for the param-list open; the mti
> rename already handled this, the pattern builder didn't); (b) **deferred class-object-arg
> binding marker** in `object_arg_addr` pattern mode (forwarding-ref param used as class arg →
> N_IGNORE marker; copy re-decides bind-vs-convert with the substituted type; passthrough
> covered, conversion errors into the self-detecting fallback). Burndown 255/11 FLAT, but
> `recipe parse failed` class GONE (reclassified). Gates all green; sweep identical to baseline.
>
> ### The 11, by body (probe evidence, env-hooks removed before commit):
> 1. **`_Rb_tree::_Alloc_node::operator()<_Arg>` ×3** (set/containerdtor/madc_ns) — body
>    `return _M_t._M_create_node(std::forward<_Arg>(__arg));`. Pattern now PARSES (fix a).
>    Remaining: eligibility carve-out for the `forward<_Arg>` body template-id (a scalar
>    dependent-CALL template-id — the Kind-1 covered shape; `resolve_copied_dependent_call`
>    already substitutes `explicit_template_args` and re-resolves member calls incl.
>    member-template instantiation) + the ref-param guard (see 4).
> 2. **`_Rb_tree::_M_insert_<_Arg,_NodeGen>` ×3** — body has `__node_gen(std::forward<_Arg>(__v))`,
>    a FUNCTOR CALL on a placeholder-typed object. ⚠ THE PARSER MIS-PARSES THIS into assignment
>    garbage `(*__node_gen) = fwd(...)` (probe: emitted C, gcc named it) — NOT self-detecting.
>    The functor-call path (parser.cpp ~19893, P2.1b) requires `operand_object_class` to resolve;
>    a placeholder receiver falls through. Fix at the parser: a dependent functor call must build
>    a dependent `operator()` member-call token (then `resolve_copied_dependent_call`'s
>    TokenMember arm re-resolves at copy: substituted receiver → `findMethodOverload("operator()")`
>    → member-template instantiate). `testfunctortmploperator.mad`'s header documents the same
>    shape for the CONCRETE case. Also note parser.cpp ~17873: a receiver class with
>    `class_has_unresolved_dependent_surface` gets its call SWALLOWED into
>    `make_dependent_call_placeholder` (a TokenInt!) — any admission must ensure that path
>    errors in pattern mode rather than baking a placeholder int.
> 3. **`_Rb_tree::_M_insert_unique<_Arg>` ×3** — needs (2)'s forward-arg handling for the
>    `_M_insert_(...)` call PLUS statement-level RETURN-CONSTRUCTION deferral:
>    `return _Res(_M_insert_(...), true);` — ctor selection on a dependent-typed arg (the
>    `_M_insert_` call's return is unknown in the pattern) → currently
>    "dependent-arg object construction" from the ttCallFunc arm. Model: the Kind-4 local-decl
>    marker, but in the return path (retbuf/objtmp plumbing). Body template-ids
>    `pair<iterator,bool>` / `pair<_Base_ptr,_Base_ptr>` are PNAME-FREE (concrete in the
>    owner scope at pattern parse) — need a `tsubst_lt_opens_nondependent_template_id`
>    eligibility carve-out (no pname in span, no nested `<`).
> 4. **`reference-param value-read` ×2 — the scalar-ref-param guard slice, with LOCAL reducers:**
>    `testfunctortmploperator` (`Gen::operator()<Arg>`) and `testmembertmplptrret`
>    (`Box::make<Arg>`, body `n.v = base + a;` — a genuine SIZED value read: the hazard is a
>    deref typed by the placeholder baked into the shared pattern). Narrow
>    `tsubst_body_has_unsupported_reference_param` (cir_builder.cpp ~14120) by what the body
>    actually does: call-argument/passthrough uses are safe (the new deferred-arg marker covers
>    class-object binding); sized VALUE READS need per-instantiation deref typing (verify
>    whether `copy_node_under`'s datadef substitution already re-types the deref — the guard
>    may predate it). Probe these two reducers FIRST — smallest, no libstdc++ noise.
>
> ### Beyond burndown=0 (for the default flip / re-parse deletion — track separately):
> the 4 PRE-EXISTING flag-on-only failures (testifconstexpr/testinvocable SIGSEGV,
> testfstream/teststdstringconv rc=1 — sweep tool finds them), the local-class ctor
> `this`-capture bug, and Phase-5 ctor mem-init tsubst (mem-inits ride the shell today).

**Rehydrate from THIS doc.** It supersedes `2026-07-01-tsubst-2B-completion-HANDOFF.md`
(2B is DONE — read only its top COMPLETE banner for the 2B root cause) and the Piece-2B
sections of the step-2 handoff. Campaign law: `.claude/rules/parse-once.md`.

---

## 0. STATE (all committed on `develop`, tree GREEN, held for /release — do NOT push raw)

- HEAD `2995c6b0` (status sync); code commits this session:
  - **`c4855344` — 2B COMPLETE**: comparison-`<` `tsubst_eligible` classifier + the real
    root-cause fix `Program::instantiating_spelling_applies_here()` (madc.h ~2080;
    gated at the TokenCLASS + TokenSTRUCT canonical-spelling stamp sites).
  - **`89ae0fe0` — Kind-4 deferred local-declaration construction** (plan Slice B of
    `2026-06-27-tsubst-construction-deferral-PLAN.md`).
- **Burndown: 243 hit / 23 fallback = 91%** (session start: 176/90 = 66%).
  `scripts/tsubst_burndown.sh` is the metric; `scripts/tsubst_flagon_gate.sh` (+
  `docs/parity/tsubst-flagon-baseline.txt`, bumped: map 9/2, set 7/3, vector 15/0,
  subscript 31/4, containerdtor 28/5, madc_ns 28/5, localclassraii 1/0) is the ratchet,
  wired into fulltest. fulltest = **674/0/0/16 exit 0**; torture byte-identical to the
  51-name baseline (verified for c4855344; 89ae0fe0 is flag-on-gated by construction).
- Untracked `mir-debug-support.md` is NOT ours — never stage it. Commit via
  `git commit -F -` heredoc; stage explicitly; single shell commands (no `&&`).

## 1. WHAT LANDED (mechanics, for code navigation)

### 2B (`c4855344`)
- `tsubst_eligible` (parser.cpp ~33820): a `tkLT` disqualifies only if it opens a real
  template-id (`template_id_suffix_end(d,i) != i`) and is not a concrete named/RTTI cast
  (`tsubst_lt_opens_concrete_cast`). Dependent template-ids still rejected.
- Root cause it exposed (NOT the old "receiver swap" theory): the basic_string range-ctor
  Tree-1 PATTERN PARSE parses local `struct _Guard` at block scope while
  `instantiating_canonical_spelling` still holds `std::__cxx11::basic_string<...>`;
  the stamp gave `_Guard` basic_string's canonical spelling; `resolve_arg_spelling_datadef`'s
  struct_map canonical scan then resolved basic_string's spelling to `_Guard` (key sorts
  first) → poisoned pair `_Args1` deduction → `forward<const _Guard&>` →
  `basic_string(const _Guard*)`. Fix: a block-scope class never takes the instantiating
  template-id ([class.local]) — `instantiating_spelling_applies_here()` =
  `!spelling.empty() && compounds.empty()`.
- Debug technique that cracked it: env-gated abort-trap at the poisoned-object CREATION
  predicate + `gdb -batch -ex run -ex bt`; then dump ALL map entries matching the stolen
  identity.

### Kind-4 slice (`89ae0fe0`)
- `CirBuilder::tsubst_relower_deferred_construction` (cir_builder.cpp ~1713, decl in
  cir_builder.h): the EXTRACTED placement-new manual pack machinery (expand pack →
  select ctor overload → per-element translated arg nodes under element substitution →
  `ctor_call_assemble`), parameterized by `this_addr` builder + `yield_this_addr` +
  `relax_class_args`. Returns NULL → placement-new caller's simple re-translate path.
- `translate_block` decl branch (~13770): pattern mode + `tsubst_args_have_pack_expansion
  (sdcl->ctor_args)` → emit storage decl + N_IGNORE marker (datadef = the class).
- `copy_cir_subtree` N_IGNORE branch (~2170): TokenDecl-origin marker → re-lower via the
  helper with `relax_class_args=true` (admits the non-pack `*this` ref-bound class arg;
  forces manual assembly). `_Rb_tree::_M_emplace_hint_unique` (`_Auto_node`) = HIT.

## 2. NEXT LEVERS (the remaining 23 fallbacks, by [why:])

1. **`>1 pack param` (6) + `non-type template param` (6) — the pair piecewise/indexed
   ctors** (`pair(piecewise_construct_t, tuple<_Args1...>, tuple<_Args2...>)` and the
   delegated `pair(tuple&, tuple&, _Index_tuple<_Indexes1...>, _Index_tuple<_Indexes2...>)`).
   These are ELIGIBILITY-clause rejections (parser.cpp `tsubst_eligible`: the one-pack
   limit and the non-type-param reject) — the biggest single family (12 of 23). Needs:
   multi-pack binding in `tsubst_method_body`'s binding construction (it currently
   assumes `pack_params` by index with single fan-out state
   `m_tsubst_copy_pack_index/_elem/_value_name` — would need per-pack state), and
   non-type (value) pack args carried through `tsubst_type_arg_packs` (they're DataDefs;
   the `_Indexes` are VALUES — see `datadef_is_nontype_constant`, the set-wall fix).
   Parse-time already instantiates these correctly (o19/o20 exist) — this is purely the
   CIR tsubst side.
2. **`template-id '<' in body` residual (9)** — real dependent template-ids in bodies
   (the Kind-3 dependent-member-type family; see
   `2026-07-01-templateid-gate-insight-HANDOFF.md`). Requires `subst_datadef`
   make_typename_type→lookup_member analogue (construction-deferral plan §6.3/Slice D).
3. **Pre-existing local-class ctor `this`-capture bug** (separate correctness track, NOT
   burndown): concrete Guard ctor mis-types its enclosing-class pointer capture
   (warning "incompatible argument type for pointer type parameter" on the flag-on hits;
   `tmp/guard_dtor_runs.mad` SIGSEGVs flag-on AND flag-off). Parser-level; reducers in
   `tmp/guard_*.mad` with gcc oracles.
4. `reference-param value-read` (1), `recipe parse failed` (1) — mop-up.

## 3. GATES per slice (unchanged discipline)

- Build 0-warn; `make -C src fulltest` exit 0 (includes warning ratchet, drift gates,
  flag-on ratchet). Flag-on gate: run WITHOUT a pipe; bump the baseline in-commit on
  PROGRESS. Burndown must move DOWN or FLAT.
- Verify hits by RUNNING flag-on (output == flag-off), never compile-only.
- Torture re-run needed only if any changed path is reachable flag-off; a change entirely
  behind `m_tsubst_pattern_mode`/`subst` is byte-identical by construction (plan §8).
- NEVER roll back the emitted program on a fallback; NEVER name-key a fix (Rule #7).

## 4. MIRRORS (synced at HEAD)

`claude_status.json` (head/current_phase/build_status), KG
`Feature{two_tree_tsubst_instantiation}`, memory `project_reparse_deprecation`,
`docs/parity/tsubst-flagon-baseline.txt`, the 2B handoff's COMPLETE banner.
