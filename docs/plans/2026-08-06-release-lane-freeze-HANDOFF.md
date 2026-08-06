# HANDOFF — release lane red: frozen-corpus instantiation-identity split (#20)

Written 2026-08-06, discovered when the owner asked why no `madc-release`
had been built since Aug 1. First `release packed` run since then
(tmp/logs/rb-20260806-205308.log): `make release` FAILS at the
forest-pack verify and the packed suite is 982/15. The failure predates
and is independent of `feature/script-toplevel-claude` (#16/#18).

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
