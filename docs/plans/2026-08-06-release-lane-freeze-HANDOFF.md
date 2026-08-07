# HANDOFF — release lane red: frozen-corpus instantiation-identity split (#20)

Written 2026-08-06, discovered when the owner asked why no `madc-release`
had been built since Aug 1. First `release packed` run since then
(tmp/logs/rb-20260806-205308.log): `make release` FAILS at the
forest-pack verify and the packed suite is 982/15. The failure predates
and is independent of `feature/script-toplevel-claude` (#16/#18).

## STATE AT COMPACTION #2 (2026-08-07) — READ THIS FIRST, THEN FINISH IT

**OWNER DIRECTIVE: resolve ALL of #20 next session.** That means: the
remaining 8 subset failures -> 0, then the full finish line (below).
No re-derivation — the five landed fixes and the two-defect verdict are
SETTLED; resume at "START HERE" under REMAINING.

- Branch `feature/script-toplevel-claude`, tree CLEAN, **ahead 9,
  ALL UNPUSHED** (owner wall-grind protocol — push only with the
  wall-fall battery): @8b77c972 ovset round-trip (H1) + @85cd4d9b
  @7b31135a docs + **session #68's five fixes**: @3e8605f1 canonical
  binding dds, @ea49eeb8 recorder slot pinning, @8c60b61d relower
  member recovery, @4fdbffd7 using-import order parity, @75c99d6f
  dd-own-name != typedef-alias, + @760de816 docs.
- **Subset: 15 failing -> 8 failing.** Defect P (fmin_use) FIXED
  (prints "7 1" rc=0). Live lanes verified green: 18-test spot check
  incl. testfreezerun + testscripttopdefer/multiret, 18/18.
- **CONTAINER STATE (current, reusable):** tmp/fb_madc = current
  bin/madc copy; tmp/fb_madc.forest = fresh 19-header freeze at the
  banked HEAD (re-freeze ONLY after parser/recorder-side edits;
  cir_builder/cir_freeze restore-side edits just need
  `cp bin/madc tmp/fb_madc`). The debug parser.o (-DMADC_DEBUG_FNTPL)
  was CLEANED (rebuilt without); rebuild it via
  `find obj -name parser.o -delete; CXXFLAGS='-std=c++11 -Wall -O0 -g
  -DMADC_DEBUG_FNTPL=1' make -C src -j8` when the inj-dump is needed
  (spews unconditionally — never run the suite with it linked).
- **PROBES (all env-gated, in-tree):** MADC_DEBUG_INTSPLIT=1 (flavor
  twins in bindings), MADC_DEBUG_ARGBIND=<substr> (bound callee formals
  per call arg), MADC_MTI_PROBE=<substr> (methodrec materializations),
  MADC_MTB_PROBE / MADC_FNTPL_PROBE (instantiation attempts),
  MADC_RRTRAP='<exact arg spelling>' + `gdb -batch -ex run -ex 'bt 18'
  --args ./tmp/fb_madc <test>` (names the route that minted a
  template-arg spelling; gdb works on the container).
- **FINISH LINE (in order, after the subset is 15/15):**
  1. Full re-freeze + subset confirm through the sidecar.
  2. THE ONE BATTERY: `make -C src fulltest` + libcxxjit stage +
     `bash scripts/run_tests.sh --exe` + `--obj` +
     `scripts/remote_build.sh release packed` (ALL of it green —
     release rc=0 incl. forest_pack.sh verify; packed ~997/0).
  3. Ship the gates: promote tmp/fmin_use.mad -> tests/ as the
     defect-P reducer (fixture: expect "7 1"), and add the cheap
     fulltest-wired cross-TU freeze-consumer gate (freeze a SMALL
     corpus, e.g. {memory,string,vector}, compile a consumer against
     it — the existing forest gates freeze the test's OWN TU and
     missed this entire class).
  4. PUSH all banked commits.
  5. The held #16/#18 merge to develop rides the release cadence
     (merge => release, needs the green `make release` from step 2).
  6. Sync mirrors (claude_status.json, ROADMAP, CHANGELOG, KG) + close
     #20.

## SESSION #68 LOG (2026-08-06 late) — WALL MOSTLY DOWN: 7-8/15 flipped

FOUR fixes banked (each own commit, trailers, all verified live-green —
live 18-test spot check incl. testfreezerun + script-mode gates 18/18):

1. **fix(parser) canonical binding dds** — TWO builtin dds for one type
   (ddINT "int" ← integer literals; ddINT32 "int32_t" ← the lexer/THE
   table `resolve_builtin_type_spelling`). Identity formers spell binding
   dd NAMES → one entity minted two identities (forward<int> vs
   <int32_t>) → pack/consumer split. New `canonical_template_binding_dd`
   (NO alias-chain walk — block-scope typedef aliases like _ValueType2
   carry one instantiation's resolution; walking them crossed
   instantiations, caught by FNTPLPROBE) at fn_template_deduce_param's
   output + instantiate_fn_template_binding entry. Probe:
   MADC_DEBUG_INTSPLIT=1 (10 corpus hits → 0).
2. **fix(freeze) recorder canonical slot** — forest_pinned_primitive_id's
   rawtype scan let slot 5 (ddINT) shadow slot 9 (ddINT32); restored
   refs/params spelled "int" where live spelled "int32_t". Prefer the
   twin the one table maps to itself.
3. **fix(cir) copied-call member-access recovery** —
   copied_call_arg_for_formal had no arm for a MEMBER-ACCESS value with
   a dependent pattern dd (this->_M_impl into _Tp_alloc_type&): struct
   value fell to the pointer-cast tail = the stl_vector.h:428
   "conversion of non-scalar value" ×2. Added the TokenMember twin of
   the N_ID recovery, gated to N_FIELD/N_DEREF_FIELD (a resolved
   (void*)(&member) cast carries the same origin token — first attempt
   over-fired and broke the string flavor). Probe: MADC_DEBUG_ARGBIND.
4. **fix(freeze) using-decl import ORDER** — restore staged imports to a
   post-pass and APPENDED them; live parses `using _Base_type::construct;`
   BEFORE the own overloads. Thaw resolution tried __alloc_traits' OWN
   custom-pointer construct (pattern polluted with the first
   materialization's use-site spelling `_Char_alloc_type` — dormant
   live!), instantiation failed, call stayed placeholder-bound →
   undefined `__alloc_traits...__construct` MIR import = most of the 15.
   Post-pass now inserts at the recorded methodrec position + first-wins
   method_map arbitration. Subset 1→7 passed on this fix alone.

UNCOMMITTED (in flight): predicate guard in typedef_alias_matches_datadef
(`alias == dd->name` → not an alias) — defect P's mechanism: thawed
tokens spell the restored dd's NAME ("int32_t"), the restored typedef
registry matches it, param typedef recording made the emitter render
`int32_t *__p` in a module that never emits that typedef (fmin_use
"unknown type int32_t"; first fix attempt at
basic_class_pattern_bound_typedef alone was insufficient — the deferred
BODY re-parse lane records through the same predicate).

DEFECT P: **FIXED** — fmin_use prints "7 1" rc=0 vs the frozen corpus
(fix 5, the typedef-alias predicate guard).

REMAINING failing subset (8; likely ONE root + one separate family):
- **START HERE — the multi-namespace same-name template chooser under
  thaw.** FIRST COMMANDS on a cold start (container, ssh -p 2299
  dev@localhost, cd /workspace/madc — no rebuild needed, the harness is
  current):
  1. `MADC_BIN=tmp/fb_madc bash scripts/run_tests.sh testautoincludecpp
     testautoincludens testautoincludestdheaders testcommontype
     testcontainerdtor testctortemplatetrait testforeachref
     testforeachscope testmadc_ns testmanipview testofstreamwrite
     testpreferdefault testsubscript testusingfnoverload testvector`
     — confirm the 7-passed/8-failed baseline.
  2. `MADC_RRTRAP='__gnu_cxx::char_traits<char>' gdb -batch -ex run
     -ex 'bt 18' --args ./tmp/fb_madc tests/testofstreamwrite.mad`
     — the minting route; walk frames INSIDE
     instantiate_template_use/instantiate_template_id to the resolver
     that CHOSE __gnu_cxx (see PRIME SUSPECT below).
  The thawed consumer materializes WRONG-NAMESPACE flavors that
  live never mints (live module: 0 gnu_cxx hits):
  - `basic_ofstream<char>`'s default `_Traits = char_traits<_CharT>`
    resolves to `__gnu_cxx::char_traits` → an entire parallel
    basic_ios/istream/ostream/iostream gnu_cxx family materializes
    (63 refs in tmp/manip.c) → its iostream trips the DOWNSTREAM
    `repeated declaration __vptr_8` (the vbase-vptr loop at
    cir_builder ~9005 and secondary_vptr_owners both land offset 8 on
    that malformed flavor — fix the flavor choice first; the layout
    dup is probably moot). Covers testmanipview + testofstreamwrite.
  - `basic_string` resolving to std::pmr::basic_string's alias family
    (`polymorphic_allocator_char` undeclared at basic_string.h:87) —
    same bare name, two namespaces. Covers testusingfnoverload +
    testctortemplatetrait, likely the autoinclude 'string' failures too.
  - EVIDENCE BANKED: both char_traits variants RESTORE (verdict 1,
    std first — so the registry is complete and ordered); the gdb
    backtrace at MADC_RRTRAP='__gnu_cxx::char_traits<char>' (container,
    gdb -batch works on tmp/fb_madc) mints the spelling inside
    instantiate_fn_template_binding -> resolve_declared_type_token ->
    instantiate_template_id -> canonical_arg_key_fragment — i.e. the
    default-arg fill built a __gnu_cxx-qualified SPELLING before the
    trap. find_template's unqualified fallback (parser.cpp ~26046):
    current_namespace() match, then global, then variants[0] — find
    which route the thawed default fill takes and why the std variant
    loses (ns context empty? a different resolver — despaced-canonical
    index / resolve_arg_spelling_datadef — consulted first?).
    PRIME SUSPECT: restored canonical spellings are DESPACED and their
    ARG fragments may drop namespace qualifiers; a thawed re-derivation
    (split_template_id_spelling -> resolve_arg_spelling_datadef on the
    bare "char_traits<char>") re-resolves the name where live still held
    the dd link and never re-derived. Reproduce with the RRTRAP gdb
    recipe, walk one frame past instantiate_template_use to name the
    resolver that produced the __gnu_cxx-qualified arg spelling.
- auto-include surfaces under thaw: `cout` (testpreferdefault),
  `intptr_t` (testautoincludestdheaders), `string` in ns_php
  (testautoincludens/testautoincludecpp — flickered across runs) —
  possibly downstream of the same chooser, verify after.
- FIXED ✓: testcontainerdtor, testcommontype, testsubscript, testvector,
  testforeachref/scope, testmadc_ns (7/15 + defect P).

FILED (fix-after-wall, dormant-live defects found this session):
- The __alloc_traits OWN construct pattern captures the FIRST
  materialization's use-site arg spelling (`_Char_alloc_type`) — a
  parse-once capture bug, dormant because resolution never picks it
  live; trips under any resolution-order change.
- The SFINAE-failed instantiation leaves the CALL bound to the hollow
  placeholder instead of recovering to the next candidate (the
  undefined-import shape instead of overload-resolution recovery).
- 51962 free-operator conflict compare uses dd NAME equality; flavor
  twins now canonicalized at the deducer, but the compare itself is
  name-keyed.

## STATE AT COMPACTION #1 (2026-08-06 end of session #67) — SUPERSEDED by #2 above; kept for the protocol notes

- Branch `feature/script-toplevel-claude`, PUSHED through @67c47f9d
  (#16/#18 + docs; batch-gated: fulltest 999/0/9skip, libc++ lane
  995/0/13skip). BANKED LOCAL-ONLY (unpushed, per owner mid-repair
  directive): @8b77c972 (hypothesis 1, the ovset round-trip) +
  @85cd4d9b + this handoff commit. Push them WITH the wall-fall
  battery, not before.
- **OWNER DIRECTIVE (repair protocol):** mid-repair, run ONLY the
  failing subset — no full suites per step; the ONE battery
  (fulltest + libcxxjit + exe + obj + release + packed) runs AFTER the
  15 flip. A closing battery I had launched was killed on this
  directive (its partial log is void; kill leftovers — an orphaned
  `warn_census.sh --check` kept spawning tests after run_tests/make
  died; find spawners via the test PID's PPID chain).
- **THE REPAIR INNER LOOP (~7 min/iteration, container):**
  1. `scripts/remote_build.sh sync build` (incremental)
  2. `ssh -p 2299 dev@localhost` →
     `cd /workspace/madc; bash tmp/fbisect.sh cstddef climits cctype
     cstring cstdio cstdlib ctime cerrno string vector list map set
     utility memory sstream iostream fstream algorithm`
     (re-freezes the full corpus into tmp/fb_madc + tmp/fb_madc.forest
     with the CURRENT binary, ~4.5 min at -O0)
  3. `MADC_BIN=tmp/fb_madc bash scripts/run_tests.sh testautoincludecpp
     testautoincludens testautoincludestdheaders testcommontype
     testcontainerdtor testctortemplatetrait testforeachref
     testforeachscope testmadc_ns testmanipview testofstreamwrite
     testpreferdefault testsubscript testusingfnoverload testvector`
     (~30 s — the 15 packed failures reproduce EXACTLY through this
     sidecar harness; verified 0/15 at the banked HEAD)
- Subset baseline WITH hypothesis 1 in place: **0/15** — H1 flips none
  of them; the second driver dominates.

## THE VERDICT (bisected, verified)

- **Defect R (the regression):** first bad commit **6a24cffa**
  "fix(namespaces): using-declared function joins the target overload
  set" (session #65, shipped in v0.68.0). Predicate: `testfreezerun.mad`
  compiled against the FULL 19-header frozen corpus
  (scripts/forest_pack_headers.txt) — **v0.67.0 (809e6152) GOOD → HEAD
  BAD** with `stl_vector.h:428:54: conversion of non-scalar value
  requested` ×2. The bisect ran `git bisect run` with
  container `tmp/fb_bisect_run2.sh`; endpoints verified by hand first.
- **Defect P (pre-existing, at least v0.67, masked):** a corpus of
  `{cstdlib, string, vector}` + a consumer that only includes `<vector>`
  and uses `vector<int>` fails with `unknown type int32_t` —
  `__new_allocator_int32_t__deallocate(…, int32_t *__p, …)` is emitted
  with no `typedef int int32_t;` in the consumer module. Minimal triple
  found by header bisection (both 2-subsets pass).
- **NOISE, not the failure:** `--run-frozen: 189 unresolved
  drained-library import(s) bound to trap stubs` is a DIAGNOSTIC and
  exits 0 (drained bodies outside the executed closure are legal;
  v0.67's green pack printed 286). The pack script actually dies at
  `out_cache=$(… testfreezerun.mad …)` under `set -e`
  (scripts/forest_pack.sh ~line 120) because testfreezerun exits 1.

## EVIDENCE (banked on the container, /workspace/madc/tmp/)

- `fmin_use.mad` — the tiny consumer; `fbisect.sh` — header-subset
  driver (freeze corpus → run consumer); `fb_bisect_run2.sh` — the
  commit-bisect runner (build + full-corpus freeze + testfreezerun).
- `fp_treediff.txt` — normalized `--dump-cir` tree diff of the SAME
  corpus+consumer at ff79e5a2 (parent, good) vs 6a24cffa (bad). The
  smoking line: one instantiation keyed
  `__ns_std___check_constructible__istd__check_constructible_ValueType2int32_tP…_0d693c66`
  (parent) vs `…ValueType2intP…_1d83072c` (bad) — **the same entity
  spelled `int32_t` vs `int`**, plus several `__mti` instantiation
  SPEC_DECLs present only on the parent side while call sites remain.
- Malformed identities in the bad full-corpus tree:
  `num_get<int32_t, istreambuf_iterator<int32_t, char_traits<wchar_t>>>`
  (wchar_t substituted as int32_t, inconsistently) — matches the
  unresolved-import list (`__ctype_abstract_base_int32_t__*`, …). The
  int32_t-flavored spellings exist at v0.67 too (grep the Aug-1 log) —
  they are defect-P-adjacent; only the SPLIT is new.

## LAYER CHAIN (verified reading, not yet fixed)

1. Instantiation identity = `overload_spelling_symbol_suffix(spelling)`
   (parser.cpp ~2169): head + FNV-1a of the raw SPELLING. Its comment
   states the invariant: "live parse, pack drain, and bound consumers
   mint the SAME symbol".
2. The spelling comes from `cpp_spelling_for_mangle` →
   `DataDef::canonical_cpp_spelling()` / `name` — a TYPEDEF-ALIAS dd
   ("int32_t") spells differently from the canonical builtin ("int").
   `DataDef::scalar_alias_of` + `mangle_scalar_spelling()`
   (include/datadef.h ~212-237) already exist for [temp.type]
   desugaring — at least one identity former bypasses them (the class
   instantiation namer spells `vector_int32_t_…` even live).
3. 6a24cffa made using-imported functions JOIN
   `namespace_fn_overload_sets` (live arm in TokenUSING::parse). Sets
   that had ONE entry now have ≥2 → `find_namespace_function_overload`
   RANKS (it early-outs at size<2) where the map binding used to win →
   a different winner whose param dds spell differently → the identity
   splits between minting sites.
4. **The thaw twin is missing the join:** the forest ns-import replay
   (flush_forest_pending_globals, parser.cpp ~21656 "ns import") rebinds
   `namespace_map[ns][name]` first-wins but does NOT register the
   imported fn in `namespace_fn_overload_sets` — a bound consumer
   resolves against a SMALLER set than the freezing parse (vecbind
   LOADED==PARSED violation). Suspected primary trigger; unverified.

## HYPOTHESES TO TEST FIRST (in order)

1. ~~Add the ovset join to the nsbind thaw replay~~ **DONE, INSUFFICIENT
   ALONE (@8b77c972, same session):** the recorder now writes
   overload-set membership (flagged `DF_NSBIND_OVERLOAD_MEMBER`, incl.
   imports that lost the first-wins map slot and previously left NO
   record) and the flush joins flagged records back into
   `namespace_fn_overload_sets`. A proven live==thaw state divergence,
   landed with a 5/5 targeted net — but the full-corpus testfreezerun
   STILL fails with the identical `stl_vector.h:428:54` pair. The
   identity split has a second driver.
2. **START HERE — find the second driver.** Build with
   `-DMADC_DEBUG_FNTPL=1` (it is a compile-time `#if`, not an env) and
   compare the RANK decisions freeze-side vs consumer-side for the
   `std::__check_constructible` call chain and the stl_vector.h:428
   members; the boundary tree diff (tmp/fp_treediff.txt) names the
   split keys. Then the deep fix per [temp.type]: the identity spelling
   must desugar scalar typedef aliases (route EVERY identity former
   through `scalar_alias_of` / `mangle_scalar_spelling`, cf. 588d9e73
   "one instantiation key for typedef'd anonymous-aggregate args").
   That likely also fixes defect P's family. Do NOT key on names
   (enum-over-strings); desugar at the dd level.
3. Defect P separately: the consumer module must emit (or desugar away)
   the `int32_t` typedef its thawed-pattern instantiations spell.

## GATES TO SHIP

- The full pack must pass again: `remote_build.sh release packed`
  (release rc=0 incl. forest_pack.sh verify; packed suite baseline
  997-ish/0 — the 15 current failures: testautoincludecpp/ns/
  stdheaders, testcommontype, testcontainerdtor, testctortemplatetrait,
  testforeachref/scope, testmadc_ns, testmanipview, testofstreamwrite,
  testpreferdefault, testsubscript, testusingfnoverload, testvector).
- A cheap fulltest-wired gate so this can't silently regrow: freeze a
  SMALL corpus that reproduces R (e.g. {memory,string,vector} — find
  the minimal set with the 428 signature via `fbisect.sh`) + compile a
  consumer against it. The existing forest gates freeze the test's OWN
  TU and missed this cross-TU consumer path entirely.
- Defect P reducer: corpus {cstdlib,string,vector} + `fmin_use.mad`.

## PROCESS NOTES (bitten this session)

- `release`+`packed` are session-end/pre-merge stages ALONGSIDE EXE/OBJ
  — they silently dropped out of the trimmed cadence Aug 1 → v0.68.0
  (owner caught it). A release commit needs a `release packed` run on
  its src content in evidence.
- Container-side `git checkout` (bisecting) poisons make for later
  rsync-based syncs: checkout-refreshed objects are NEWER than synced
  sources (rsync preserves NAS mtimes) → `build rc=0` with a STALE
  binary. After any checkout session: `make -C src clean` build, and
  nm-check a new symbol before concluding a fix failed.
- v0.68.0 is PUBLISHED with this lane red (unknowable at publish time —
  the lane hadn't run since Aug 1). The release binary shipped… was NOT
  shipped: `make release` fails, so no v0.68.0 madc-release exists.

## SETTLED — do not re-litigate

- 6a24cffa's LIVE semantics are correct per [namespace.udecl] (gate
  testusingfnoverload, both flavors) — do not revert it; fix the
  identity/thaw side.
- The bisect verdict and the two-defect decomposition above.
- #16/#18 (top-level defer/:=) are DONE on feature/script-toplevel-claude
  (commits 11c983a6 + 126f03eb, gates testscripttopdefer/
  testscripttopmultiret) — that branch's merge to develop WAITS on this
  fix so the merge can ride the release cadence (merge ⇒ release ⇒
  needs a green `make release`).
