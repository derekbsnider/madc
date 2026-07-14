# CODEX HANDOFF — bound-compile performance: the cir_build bucket and the INSTANTIATE bucket

**From:** Claude, 2026-07-14 (v0.35.0 release sitting). **For:** Codex (GPT-5.6).
**Base:** `develop` @fdbb4d78 (v0.35.0). Cut `feature/forest-perf-<slice>-codex` off develop, one slice per branch.
**Owner mission:** reduce bound-compile time — the INSTANTIATE bucket, and **especially the
cir_build bucket, which the forest makes SLOWER than a live parse**.

This doc is self-contained; you have no access to prior session memory. Everything
you must not re-derive is in SETTLED. Read these before coding:
`docs/plans/2026-07-13-instantiate-bucket-plan.md` (the Slice A/B/C charter — Slice B
is yours), `.claude/rules/parse-once.md`, `.claude/rules/backend-strategy.md`,
`docs/plans/2026-07-14-forest-recompress-dictionary-HANDOFF.md` (the decode-cost
composition this release added).

## THE EVIDENCE (measured 2026-07-14 on v0.35.0, same -O2 packed binary, testsubscript)

`bin/madc-release --show-stats tests/testsubscript.mad` (bound) vs
`bin/madc-release --no-forest-bind --show-stats tests/testsubscript.mad` (live parse):

| phase (`--show-stats`) | live -O2 | bound | delta |
|---|---|---|---|
| read | 0.003 | 0.001 | |
| lex | 0.171 | **0.161** | ≈0 — but bound lexed only 279 tokens vs 189,102: this bucket is ~all BIND work (restore_decls, arena/type materialization, intern rebind), not lexing. Attribute it. |
| parse | 0.342 | 0.101 | −0.241 — the forest's win |
| — instantiate (inside parse) | 0.279 (6801 calls) | **0.156 (3804 calls)** | still 155% of bound parse time |
| — tsubst bodies | 35 hit / 0 fb | 34 hit / 2 fb | |
| **cir build** | **0.265** | **0.368** | **+0.103 — the forest makes cir_build 39% SLOWER** |
| c2mir compile | 0.017 | 0.019 | |
| other (setup/link) | 0.064 | 0.071 | |
| **total in-process** | **0.862** | **0.721** | −0.141 |

Wall trend (`docs/perf/forest-timings.tsv`, 5-run medians on testsubscript):
bound 0.560 (2026-07-13, pre-campaign corpus) → **0.715** (v0.35.0). Two known
contributors landed in between — attribute before optimizing:
1. **Family-D campaign corpus growth**: pack drops 483→308 means ~175 MORE drained
   bodies frozen (records raw grew to 61.8 MB) — more records to decode + materialize.
2. **Pack compression decode** (task #37, deliberate, owner-approved): per-segment
   zstd + segment transforms + intern-spine decode. Measured composition on the full
   corpus: records inverse byte-plane transpose ~26 ms (SSE2 tiled) + zstd frame
   decodes ~30-40 ms + intern spine ~7 ms once per process + children delta ~3 ms.
   Slice-1-era measurement put the whole decode delta at ~+50 ms; v0.35.0 total decode
   is ~+80-130 ms worst-case. These land inside the cir_build (unit_segment loads
   during materialization) and lex (bind) buckets.

## MISSION, in priority order

### 1. Attribute, then shrink, the bound cir_build bucket (0.368s vs 0.265s live)

The bucket mixes at least: segment decode (zstd + inverse transforms, lazy per unit
via `CirFrozenForest::unit_segment`, memoized in `_segs[unit]`), frozen-record →
`cir_node` materialization (`node_for` walks), and translate-time work identical to
live. Callgrind the bound run (they complete within the CPU cap; see discipline
below) and produce a composition table FIRST — the lookup-churn lesson (plan doc
SETTLED) is that name-substring attribution lies; verify per-callee.

Candidate levers — VERIFY against the composition, do not assume:
- Decode double-work: does any unit decode more than once despite the memo? Does
  materialization touch units/segments the consumer never needs (POSITIONS is only
  needed for diagnostics — is it decoded eagerly)?
- Materialization width: does `node_for` materialize whole units where the consumer
  binds a subset? (Records are copied out via `cir_freeze_read`-style whole-frame
  reads — a per-record lazy view may pay, but weigh against the validation pass.)
- The bind bucket (0.161s "lex"): forest_restore_decls / arena materialize_from_arena
  / template-state restore — what dominates? Restore work proportional to the WHOLE
  corpus on every compile is the smell to hunt (the consumer typically binds a
  fraction).
- Do NOT regress the 9.26 MB binary or the compression design to buy wall time:
  codec/transform changes require re-running the size gates (census below) and any
  size-vs-speed trade >2x in either direction is an OWNER DECISION — ask first.

### 2. INSTANTIATE bucket: execute Slice B of the instantiate-bucket plan

`docs/plans/2026-07-13-instantiate-bucket-plan.md` §"Slice B" is the charter: class
templates today instantiate by RE-RUNNING THE PARSER over substituted saved tokens
(`instantiate_template_use` → `parseKeyword`) — measured 52% of the whole bound
compile (1.81B Ir of 3.465B; 251 class-body parses driven by just 7 consumer
specializations). Extend the parse-once model (g++ tsubst; the finite-KIND spine in
`.claude/rules/parse-once.md`) to the class KIND.

- **Design sitting FIRST, no code in the same sitting** (the charter's rule). The
  design doc enumerates every side effect TokenSTRUCT/TokenCLASS::parse produces
  (DataDefCLASS, members, method_map Variables, vtables, layout, type_aliases,
  nested types, struct_map/datatype_map writes, template registrations, forest taps)
  — read the parser, don't guess.
- Implementation follows the ratchet model: eligibility starts NARROW (container
  shapes), everything else falls back to today's token parse with a tallied `[why:]`
  reason (`--show-stats` counter: pattern-instantiations vs parse-instantiations);
  the fallback tally only goes down or flat, never up.
- The instantiated result must be INDISTINGUISHABLE from the parsed one: same
  struct_map/datatype_map entries, layouts (sizeof/offset oracle vs g++), mangled
  symbols; whole-suite green + bind byte-identity.
- Win lands on BOTH live and bound (this is front-end work, not forest-specific).
- Option C (explicit-instantiation prelude TU) stays OWNER-GATED — do not implement.

### 3. Bench rows at every milestone

`bash scripts/forest_phase_bench.sh "<note>"` at load < 2 (check `uptime`; the NAS
is shared). The TSV is the trend of record; per-sitting wall claims don't count.

## GATES (per commit — non-negotiable)

```
make -C src fulltest                                   # 695/0/0/16 expected
make -C src release                                    # rc=0
MADC_BIN=bin/madc-release bash scripts/run_tests.sh    # 695/0/0/16 expected
bash scripts/forest_bind_gate.sh                       # 18/18, bound == live == g++
ls -la bin/madc-release                                # ~9.7MB — blob PRESENT
```
(Run as separate commands — never chain with `&&`; builds/long suites as background
tasks.) ⚠️ TRAP: a FAILED pack leaves a blob-less binary and the packed suite
SILENTLY passes as a live-parse run (no magic at EOF → fallback). ALWAYS verify the
binary size / blob presence alongside packed results. A census tool is trivial to
re-derive: footer = last 32 bytes (dir_offset u64, blob_size u64, seg_count u32,
version u32, magic "MADCSNAP"); 40-byte directory entries (seg_id, kind, offset,
comp_size, raw_size u64s, codec, flags).

## TOOLS

- `--show-stats` — the phase table above; also the tsubst hit/fallback profile.
- `--no-forest-bind` — same packed binary, forced live parse (the A/B lever).
- `MADC_MTI_PROBE=_` — logs consumer-side deferred-body derivations.
- Callgrind discipline: bound runs complete within the box's ~120s per-process CPU
  kill; LIVE -O0 runs may truncate — compare per-site call counts, never totals;
  re-profile the mechanism after any change before benching it.
- `scripts/perf_vs_gcc.sh <file>` — GCC is also the performance baseline.
- Scratch/reducers in `tmp/` (gitignored) with the ORIGINAL flags — flagless
  reducers mask real-header bugs.

## SETTLED (do not re-derive, do not violate)

- **Forest = SAVE STATE / LOAD STATE.** LOADED must EQUAL parsed. Never re-parse,
  re-derive, or invent a parallel format on the bind path. No new record families
  without the owner's explicit sign-off. The TU-root fence stays (consumer
  specializations are TU state; they never silently enter header groves).
- **The packed suite is the arbiter.** `--run-frozen` and spot-checks are NOT
  sufficient validation (this has burned two prior slices). The c2mir check gate is
  not the last arbiter either — a fix can advance bodies from check-drop to
  GEN-fatal; only the release pack + packed suite catch that.
- The v0.35.0 compression design (per-segment zstd L15 pack / default dev; segment
  transforms delta32+byteplane; intern spine compressed in the release pack ONLY,
  raw in dev freezes) is owner-approved and SETTLED — don't "simplify" it away.
  ZDICT dictionaries were measured and refuted; don't resurrect them.
- Mechanical/container levers on the bound wall were EXHAUSTED at the 0.56 plateau
  (verdict 2026-07-12); the plateau's ceiling breaker is Slice B. The new decode
  cost (+~0.1s) is the one NEW mechanical surface since that verdict — that's why
  mission #1 is attribution first.
- Fn-template failed-attempt filtering is a DEAD lever (~30M Ir). Don't build it.
- Fix at the deepest layer; no name-keyed special cases (enums/type predicates, not
  string compares); helpers for multi-condition checks; tabs; C++11.
- Branch `feature/…-codex` off develop; never `git add -A` (a foreign
  `mir-debug-support.md` sits untracked at the repo root); commit early; the tree
  you inherit is green — keep it that way between commits.
- Update `claude_status.json` + `CHANGELOG.md` at milestones; end with a hand-off
  note (branch, tree state, validation run, remaining work).

## ACCEPTANCE

- Mission 1: a measured composition table of the bound cir_build + bind buckets,
  then landed reductions with the full gate matrix green and a bench row per landing.
  Target: bound testsubscript wall back under ~0.60s without touching the binary
  size, OR a written finding that the decode/materialization floor is X ms and why.
- Mission 2: the Slice B design doc (plan-doc appendix or standalone in docs/plans/),
  then ratcheted implementation slices, each with the
  pattern-vs-parse instantiation counter moving the right way and suites green.
  Structural target from the charter: the ~52% class-body re-parse share becomes a
  substituted-tree walk; expected shape = BOTH live and bound totals drop.
- Honesty rule: report what you measured, including refuted hypotheses — a lever
  that doesn't pay is a finding, not a failure.
