# CODEX HANDOFF — implement class-KIND parse-once, slices B0–B6 (all seven)

**From:** Claude, 2026-07-14. **For:** Codex (GPT-5.6).
**Base:** `develop` @6c84a226. **Branch:** `feature/class-parse-once-codex` off develop —
ONE branch, ONE commit per slice, each commit's tree fully green. Hand back for
independent verification at the B1, B3, and B6 checkpoints (below) before those
states merge to develop.
**Owner mandate:** implement ALL SEVEN slices of the approved design.

## THE AUTHORITATIVE SPEC

`docs/plans/2026-07-14-class-kind-parse-once-DESIGN.md` — your own accepted design.
It is SETTLED; this handoff adds execution order, gates, and traps, never design
changes. If implementation reveals a genuine design defect, STOP, record it in the
handback note, and redesign only that point explicitly — do not silently diverge.

**Approvals in hand (§15 banner, recorded @6c84a226):** B0 is green-lit, and the B1
format extension is OWNER-APPROVED as specced: `ClassPattern` rides the existing
`CIR_TMPLK_CLASS`/`CIR_TMPLK_PARTIAL` records via a pattern-payload slice in the
existing `TEMPLATE_PAYLOAD` segment, `CIR_FOREST_FORMAT_VERSION` bump, no new
segment, no new top-level record KIND. Bind-time reparse of a restored body remains
forbidden in every slice.

## WHY (measured evidence, v0.35.0 era)

- Class-template instantiation re-runs the PARSER per specialization: 251
  `parseKeyword` class-body calls from 7 consumer specializations = 1.81B of 3.465B
  instructions (52%) on bound testsubscript.
- Current phase table (`--show-stats`, packed -O2): bound total ~0.62 s wall,
  parse 0.101 s with instantiate 0.156 s (155% of parse), cir_build 0.283 s
  (post-columnar-records). Live -O2 (`--no-forest-bind`): instantiate 0.279 s of
  0.342 s parse. Slice B lands on BOTH.
- Mechanical/container levers are exhausted (0.56-plateau verdict 2026-07-12;
  Mission-1 columnar records closed the decode excess). This ladder is the
  remaining structural lever.

## SLICES (design §12 — acceptance per slice)

1. **B0 — registration/observability foundation.** Shared shell / method-signature /
   base-flatten / completion helpers used by the sole-parse lane (behavior-neutral);
   transactional registration journal; `--show-stats` class-instantiate counters
   (`pattern / parse / cache / opaque` + ranked `[why:]` parse profile, design §10
   definitions exactly — cache hits are NOT pattern hits); a burndown script
   parallel to `scripts/tsubst_burndown.sh` with a checked-in baseline; the exact
   vector<int32_t>-chain class-parse census (which KINDs the 251 parses need).
   Acceptance: byte-identical behavior (full matrix green), counters live, census
   written into the plan doc or a sibling census file.
2. **B1 — pattern capture + forest carry.** `ClassPatternArena`; one-time capture/
   normalization at template-definition parse; serialization per the approved format
   (§9); restore; semantic-fingerprint round-trip tests (§11.1). ALL patterns stay
   ineligible — zero instantiation-path change. Acceptance: capture→freeze→restore
   fingerprint equality on the container chains; full matrix green; format bump
   lands here (see TRAPS).
3. **B2 — basic structural instantiation.** Narrowest eligibility (§12.3): concrete/
   template-param unary types, aliases, scalar/pointer/array members, simple
   methods, forward completion. Eligible failures are LOUD after journal rollback —
   never a parser retry. Acceptance: first nonzero pattern-lane count, parse count
   ratchets down or flat, equivalence oracles green (§11.2: sizeof/offset/mangled
   symbols vs g++/clang), full matrix green.
4. **B3 — vector closure (THE PAYOFF — bench checkpoint).** Everything the B0 census
   demands for the vector chain (§12.4). Acceptance: the measured 7-specialization
   class-parse count falls materially to the pattern lane; BOTH live and bound
   testsubscript instantiate + total wall DECREASE (bench row at load<2); binary
   size and compression policy unchanged; full matrix green.
5. **B4 — string closure.** Defaults, static members, unions, anonymous aggregates
   per the string census (§12.5).
6. **B5 — map/set/list closure.** Deeper nested/member-template + virtual/friend
   surfaces (§12.6).
7. **B6 — reason burn-down to ZERO.** Friends/defaulted comparisons, dependent value
   expressions, explicit/partial-spec edges, out-of-line nested types — by generic
   KIND until the suite class-parse tally is 0. Then delete force-legacy
   scaffolding; the deletion of the obsolete class token-reparse machinery is a
   SEPARATE, separately-reviewed change (hand it back, do not fold it into B6).

No slice adds a name-keyed exception (a template appearing only in an ineligibility
row is unfinished coverage, not a special case). Widening is per-KIND, generic.

## GATES (per slice commit — design §13)

```
make -C src fulltest                                   # includes unit tests + bind gate; expect 695/0/0/16
make -C src release                                    # rc=0
MADC_BIN=bin/madc-release bash scripts/run_tests.sh    # expect 695/0/0/16
ls -la bin/madc-release                                # blob PRESENT (~9.7MB; 9,708,520 pre-B1)
```
Plus: the class-parse ratchet (B0's script) — parse count never increases, a new
`[why:]` reason class fails, an admitted-pattern failure is a test failure. The
tsubst ratchet stays at 0. One final heavy pass per slice — do NOT re-run a suite
on content already proven green (a checklist is not a reason; cite the green run).
Run commands separately (no `&&` chains); long builds/suites as background tasks;
scratch in `tmp/`.

## TRAPS (paid for — do not rediscover)

1. **Blob-less binary:** a FAILED pack leaves the packed suite silently running as
   live parse. Verify blob presence (binary size / footer magic "MADCSNAP" in the
   last 32 bytes) with every packed result.
2. **B1's format bump invalidates every existing .msnap** (version reject → live
   fallback). Re-freeze ALL tmp corpora you use for probes after B1 lands; a stale
   corpus mixing old/new format gives phantom results. The dev drain-ladder corpus
   is `tmp/_bf3.cpp` (43-byte `#include <fstream>`); freeze via
   `bin/madc --freeze=tmp/<name>.msnap tmp/_bf3.cpp`.
3. **Bump `CIR_FOREST_FORMAT_VERSION` (forest payload), NOT the container's
   `SNAPSHOT_FORMAT_VERSION` (v2)** — the container layer (segments, codecs,
   transforms) is untouched by this ladder.
4. The box hard-kills processes at ~120s CPU regardless of script ulimits. Bound
   callgrind runs fit; live -O0 runs may truncate — compare per-site counts, never
   totals; re-profile mechanism after changes before benching.
5. The packed suite is the arbiter; `--run-frozen` and spot-checks are NOT
   validation. The c2mir check gate is not final either (check-drop → GEN-fatal
   advances exist); only release pack + packed suite catch those.
6. `pack_gate_note` prints defective item SYMBOLS — use it; log-adjacency
   attribution has burned a rung before.
7. Never `git add -A` (foreign `mir-debug-support.md` untracked at repo root).

## CHECKPOINT / HANDBACK PROTOCOL

- After **B1**, **B3**, and **B6**: push the branch, update `claude_status.json`
  (+ CHANGELOG at B3/B6), and write a short handback note (branch, commits, what
  was validated with real counts, remaining work). Claude/owner independently
  verifies before that state merges to develop. Between checkpoints, keep landing
  slices on the branch.
- If blocked or a design defect surfaces: stop at a green commit, write the finding
  (what you measured, what refutes/blocks), hand back early. A refuted lever or a
  blocked slice honestly reported is a good outcome; a silent workaround is not.

## SETTLED (do not re-derive, do not violate)

- The DESIGN is settled and owner-approved (§15). Forest = SAVE/LOAD STATE:
  LOADED == PARSED; no bind-time reparse; no parallel format; TU-root fence stands.
- The v0.35.0 compression design (per-segment zstd, transforms, intern-spine
  pack-only) and Mission-1 columnar RECORDS are settled — no changes to buy timing.
- Option C (explicit-instantiation prelude TU) stays owner-gated. Fn-template
  failed-attempt filtering is a dead lever. Early-demand aggregate filtering is
  refuted (758/794 admitted).
- Fix at the deepest layer; enums/predicates over string compares; helpers for
  multi-condition checks; tabs; C++11; one implementation per concern.
- Suite counts live in docs/test-status.md; current baseline 695/0/0/16 live+packed.
