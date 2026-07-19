# CODEX HANDOFF — pattern-lane perf rungs (close B3's wall gate)

**From:** Claude, 2026-07-16. **For:** a fresh Codex session.
**Branch:** `feature/class-parse-once-codex` @b2b2cf5e (pushed). Base stays develop.
**Prereq reading (in order):** this doc;
`docs/plans/2026-07-16-class-pattern-capture-fixes.md` (the five defects fixed,
the fences, the measured state — SETTLED, do not re-litigate);
`docs/plans/2026-07-14-CODEX-HANDOFF-class-parse-once-B0-B6.md` (parent ladder:
gates, traps, checkpoint protocol — all still apply).

## CODEX HAND-BACK (2026-07-16, stopped before R3)

The owner requested an immediate hand-back. Do not treat R1-R2 as having
cleared their per-rung gates yet, and do not resume without a new instruction.

- Pushed commits: R1 `f0439942`, R2 `2acfaf60`, and the required R1 keying
  correction `b2b2cf5e`. The corrected registry keeps one map keyed by the
  bare-name `uint32_t`; each value holds namespace variants and a
  `DataDefCLASS*` owner sub-index. Hot owner lookup performs no composite
  string construction or pool interning. Owner-granular journaling avoids
  copying every accumulated owner on first transactional write.
- The required clean A/B exposed a regression in the original R1+R2:
  `ea4f4021` live 2.373986487 s / bound 0.519055262 s versus `2acfaf60` live
  2.445673476 s / bound 0.539002492 s (medians of 5, load below 2). Live
  counters were unchanged at 6790 calls (2646 class, 2604 alias; lane
  132/304/682/153). Bound calls improved only 3550 -> 3542 (class 1234 ->
  1230, alias 1249 -> 1245; lane 77/160/291/55 unchanged). The TSV contains
  these baseline and original R1+R2 rows; derives remained live 217 / bound
  184 in both revisions.
- After the integer-key/owner-journal correction, the live median was
  2.331096573 s at load 1.24 before / 1.34 after (samples 2.326683537,
  2.331096573, 2.383025566, 2.452833944, 2.321633888). A focused 20-owner
  callgrind workload fell from 44,496,251 to 41,057,303 instructions (-7.73%).
  A preliminary corrected bound median was 0.516307467 s, but that run
  predates the final owner-granular journal follow-up. Re-run bound on exact
  `b2b2cf5e`; the per-rung counters-and-bench acceptance is still open.
- Exact-HEAD focused tests are green: `test_class_pattern` 15/15 with 643
  assertions and `test_cir_freeze` 36/36 with 751 assertions. A clean
  pre-follow-up fulltest was green at 697/0/0/16, all 18 bind gates, and the
  equivalence/oracle gates. The exact-HEAD fulltest was interrupted during
  relinking when the owner requested wrap-up. Therefore exact-HEAD bound,
  packed 697/0/0/16 + blob, and binary-size gates remain unverified.
- R3 was not committed. Its four-file WIP is stash commit `42fb2800`, currently
  `stash@{0}` with message `wip-r3-resolver-memo`, based on `2acfaf60`. It
  contains `include/madc.h`, `src/madc.cpp`, `src/parser.cpp`, and
  `tests/unit/test_class_pattern.cpp`. R4 and R5 were not started. Leave the
  stash intact.
- A `SIGXCPU` during the full `testsubscript` callgrind run came from madc's
  internal `RLIMIT_CPU` control via `MADC_CPU_LIMIT`, not a host CPU limit.
  `src/madc.cpp` defaults it to 60 seconds; `MADC_CPU_LIMIT=0` disables it.
- Foreign untracked `mir-debug-support.md` remains untouched. Never stage it.

## STATE (verified 2026-07-16, all pushed)

- Correctness: fulltest rc=0; bind gate 18/18 incl. subbind (the owner's bar);
  class_pattern_equivalence structural == legacy == GCC == Clang; packed suite
  697/0/0/16 + blob (10.33MB); tsubst ratchet green. FOREST FORMAT v31 —
  every pre-existing `.msnap` version-rejects; re-freeze anything you probe with.
- Flagships pattern-serve at BIND: bound testsubscript 77 pattern / 160 parse /
  291 cache (pre-fix 54/199/338); `vector<int32_t>` through the lane;
  live == bound byte-identical.
- Bench (load 1.15): live 2.194 / bound 0.619. Pre-ladder floor: 2.172 / 0.593.
- Two deliberate fences (BOTH are yours to lift, in order, gated):
  1. PACK FENCE — `pack_recording` forces the parse lane at instantiation
     (parser.cpp, `if (pack_recording) force_legacy = true;`). Reason: the pack
     DRAIN derives every body; member-template OVERLOAD replay picks the wrong
     overload (`vector::_M_data_ptr` — c2mir "conversion of non-scalar" in the
     drained body; subbind caught it).
  2. LIVE LANE OPT-IN — `Program::class_pattern_live_capture` default false
     (env `MADC_CLASS_PATTERN_LIVE=1`). Lazy capture works (2nd-demand trigger,
     doomed pre-filter, cycle guard) but the lane measures net-negative live:
     2.30s vs 2.18s legacy median. NOT capture cost (captures=74 at threshold 3
     still left +100ms) — per-instantiation lane cost.

## THE MISSION

Make the pattern lane's per-instantiation cost decisively cheaper than a body
parse, fix the overload replay, lift both fences, close B3's wall gate:
**bound < 0.593 s and live < 2.17 s (medians of 5, load < 2), full matrix green.**

## RUNGS (one commit per rung, tree green at every commit)

### R1 — nested-template registration churn [biggest, measured suspect]
`register_basic_class_pattern_nested_templates` (parser.cpp ~5325) runs per
pattern-lane instantiation and `register_template(td, false)` pushes a FULL
TemplateDef copy into `template_map[bare_name]` per specialization (owner_class
differs each time → the variant scan never matches → unbounded growth), and
every later `find_template` of that bare name (e.g. `rebind`) linear-scans ALL
of them. Direction (design within it, don't shim): member templates are
owner-scoped — resolution always carries owner_hint — so give them an
owner-scoped registry (on DataDefCLASS, like class type_aliases) or an
owner-indexed variant lookup, instead of the flat bare-name vector. Gate: the
`--show-stats` call-site counters and bench BOTH improve; no correctness gate
may regress.

### R2 — journal namespace walk
`ClassRegistrationJournal` begin/commit/rollback loops EVERY `namespace_map`
entry per outermost open (one open per pattern-lane hit). Make per-namespace
transactions lazy: arm a namespace's transaction on FIRST WRITE to it (the
transactional container machinery from @89813d5b already exists — extend it),
so an instantiation touching only `std` pays one namespace, not all.

### R3 — resolver re-derivation
`BasicClassPatternResolver` gets a FRESH `resolved[]` memo per top-level call
(parser.cpp ~5679): sibling aggregates (vector / _Vector_base / _Vector_impl)
each re-derive the same dependent chains (allocator, traits, rebind) through
the FULL dispatch (`instantiate_pattern_template` → `instantiate_template_use`,
InstTimer-counted at every depth). Memoize across calls — key by (pattern
identity, type-pattern id, binding-args fingerprint) at Program level, or
thread one memo through a whole consumer chain. Invalidation: entries are
concrete TokenDataType* results; they never mutate, but a rolled-back LOUD
failure must not leave stale entries — scope the memo to committed
instantiations.

### R4 — member-template overload replay (CORRECTNESS — the pack-fence blocker)
The replay of same-name member-template overloads does not reproduce live
overload selection (`_M_data_ptr`: raw-pointer arg must pick the `_Up*`
overload, not the fancy-pointer one — live's __oN rank model does this).
Reproduce WITHOUT touching the fence: `MADC_CLASS_PATTERN_LIVE=1` + a live TU
that pattern-instantiates vector (2 distinct specs) and calls `.data()`;
compare against g++/clang per the methodology rules. Fix the rank/selection in
the nested-recipe replay (generic by KIND — no name keys). Acceptance: with
BOTH fences locally lifted (delete the `pack_recording` force-legacy line and
the eligibility `UnsupportedMemberTemplateOverloads` fence), subbind + the
full matrix are green. Land the fence removals IN THIS RUNG's commit only if
green; otherwise keep the fences and hand back with the finding.

### R5 — flip defaults + close B3 (checkpoint, owner sign-off)
When R1–R3 make the lane net-positive (A/B: `MADC_CLASS_PATTERN_FORCE_LEGACY=1`
vs default, medians of 5, same binary), flip `class_pattern_live_capture`
default-on; with R4's fences lifted, re-freeze, re-pack, re-bench. Update the
lazy-capture unit test defaults. Hand back BEFORE merging anything to develop:
Claude/owner independently verifies the B3 close (bench row, packed suite,
binary size vs 10.33MB — do not grow it).

## TRAPS (paid for — do not rediscover)

1. FOREST v31: every old `.msnap` version-rejects to live parse. Re-freeze all
   probe corpora (`bin/madc --freeze=tmp/x.msnap <tu>`). A stale corpus makes
   bind runs silently live — verify with the counters (`class patterns ....
   N materialized / M deferred` must be nonzero on a real bind).
2. The cycle guard (`class_pattern_inst_in_progress`) is LOAD-BEARING: without
   it live pattern instantiation stack-overflows on cyclic dependency webs
   (bound never shows it — restored state pre-satisfies lookups). Never remove
   it while optimizing R3.
3. Capture MUTES cerr and rolls back diagnostics: debug ONLY through
   `MADC_CLASS_PATTERN_PROBE=<substr>|*` (pipeline-wide: capture detail,
   normalize-fail sites, freeze-emit, restore, materialize fail-line,
   eligibility reject sites). In-capture probes print to cout.
4. Nested capture inside a pattern instantiation is FORBIDDEN (journal depth
   gate at the lazy-capture site): a nested isolate journal does not snapshot,
   so its rollback cannot undo the capture parse.
5. The packed suite is the arbiter; blob presence with every packed result;
   `--run-frozen` and spot checks are NOT validation. Bench rows only at
   load < 2 (`uptime` first).
6. Never `git add -A` (foreign `mir-debug-support.md` at repo root); commits
   via `git commit -F -` heredoc with a BARE `EOF` terminator; no `&&` chains;
   builds/suites in the background; scratch in `tmp/`.
7. When the owner says wrap up: STOP CODING — commit, push, handback. An
   uncommitted tree is one `git checkout` from oblivion (it has now happened
   twice).

## SETTLED (do not re-derive, do not violate)

- The five capture/restore fixes and both fences (see the 2026-07-16 doc) are
  settled. Forest = SAVE/LOAD STATE: loaded == parsed, no bind-time reparse,
  no parallel formats. The B0–B6 design (§15 owner approvals) stands.
- Widening is per-KIND and generic; a fix keyed on a template NAME is wrong.
- One heavy pass per rung; never re-run a suite on content already proven
  green (cite the green run instead).
