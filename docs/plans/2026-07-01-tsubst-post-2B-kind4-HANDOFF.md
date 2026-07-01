# tsubst post-2B + Kind-4 — HANDOFF for fresh context (2026-07-01, evening)

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
