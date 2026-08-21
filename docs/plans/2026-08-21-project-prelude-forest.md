# Project prelude forest — parse the embedded headers once per project

**Status: PLAN — awaiting owner approval. Nothing here is implemented.**

## The problem (measured, 2026-08-21, container, interleaved A/B)

Under `--project`, every madc-dialect TU is a fresh `Program`
(deliberate: file-scope isolation, per-TU flags — same isolation the C
TUs rely on). On an **unpacked** binary each Program live-parses the
embedded namespace headers its prelude pulls (bits/std_format, ns_php,
ns_ui, ns_madc — real C++ surfaces). The monolithic adventure paid that
once; the 11-TU project pays it eleven times:

| shape                                   | dev bin/madc | packed release |
|-----------------------------------------|-------------:|---------------:|
| adventure, single-TU monolith           |        0.7s  |              — |
| adventure, 11-TU --project              |        5.0s  |              — |
| 2-TU probe, trivial (println only)      |        0.48s |              — |
| 2-TU probe using php::/ui::/madc::      |        1.08s |          0.10s |

- ~0.3–0.4s per namespace-using TU; zero penalty for trivial TUs.
- fulltest's A10 adventure gate (99 invocations): 99s → 8m23s.
- The packed binary is UNAFFECTED (each TU binds the embedded forest —
  LOADED == parsed). SMAUG's 51 C TUs are unaffected (no dialect
  prelude). Correctness is unaffected (94/94 byte-identical).

So: shipped quality is fine; the costs are the dev loop (5s per game
run) and the merge-wave battery (+7 min), and they grow with any future
dialect multi-TU work.

## The design (recommended shape)

**Freeze the PRELUDE once, bind it N times — inside the project lane,
using the existing freeze + probe/bind machinery verbatim. No new
sharing mechanism, no new cache type.** (`no-parallel-implementations`:
the frozen-forest container IS the one designed vehicle for "parsed
state crosses Program boundaries"; live products can't be shared
directly — they live in per-Program pools behind activate_token_pools.)

1. **A synthetic prelude TU.** The engine synthesizes a tiny dialect
   source that touches every auto-include surface (bare
   print/println/format, php::, perl::, python::, ruby::, js::, rust::,
   ui::, madc::, cin, stderr) — generated FROM the auto-include table,
   never a hand-kept list (`design-principles`: data drives it). Only
   system/embedded headers land in it, so no user code can leak into
   the shared container and shadow per-TU state.
2. **Freeze it** with the existing `madc_cir_freeze` (arena-enabled
   parse → translate → serialize) to a scratch container:
   `tmp/prelude-<config-hash>.forest` (scratch-files rule: tmp/ only).
3. **Bind it for every TU**: `project_parse_all` sets each TU Program's
   `forest_bind_path` to the container (probe arm 0 — explicit path
   beats every discovery arm). The producer-config exact-match gate
   (`forestMatchExact`) is the staleness/invalidation mechanism —
   a rebuilt binary or changed config MISSES and re-freezes; never
   an mtime heuristic of our own.
4. **Cache across runs**: if the container exists and the config gate
   accepts it, skip the freeze. Run 1 pays parse+freeze; every later
   run binds only. Expected: game invocation 5.0s → ~1s; the A10 gate
   → ~2 min (from 8m23s).
5. **Precedence**: an explicit user `--forest-bind` / a packed binary's
   own forest wins — the prelude container is only built/used when the
   normal probe chain found nothing (the packed lane must keep testing
   its own pack).

### Thread-safety contract (owner law)

The container is read-only mmap'd state, opened per Program — the same
contract the packed binary's embedded blob already has (concurrent
readers safe; no mutation after freeze). TU compilation stays
sequential in the project lane. The cross-run cache file is written
once via atomic rename (write to `<path>.tmp.<pid>`, rename), so a
concurrent second madc either sees the complete container or misses
the probe and freezes its own.

## Why not the alternatives

- **Share live parsed products across Programs** (no serialization):
  fights the pool-activation architecture; the forest serialization
  exists precisely because unit products live in per-Program pools.
  The long-term home for a shared immutable header layer is the
  2026-06-09 front-end representation refactor
  (`docs/plans/…header-forest…`) — this plan is the project-lane slice
  that reuses its shipping vehicle (the container) today.
- **Freeze from TU[0]'s own parse** instead of a synthetic prelude:
  risks user-code units leaking into the shared container, and TU[0]'s
  header coverage may under-serve other TUs. The synthetic prelude is
  total by construction.
- **Build a dev sidecar pack in `make -C src`** (probe arm 3,
  `<exe>.forest`): biggest win (fixes single-file dev startup too —
  the R4 arc) but flips the ENTIRE dev suite from live-parse testing to
  bind testing — a testing-surface change that is an explicit OWNER
  decision, not a side effect of this fix. Recorded as a separate
  lever; not part of this plan.
- **Point the fulltest gate at the packed binary**: fulltest tests the
  binary it builds. Rejected.

## Slices

- **S0 — verify the load-bearing assumptions (recon, no code):**
  (a) bind granularity — a bound TU that needs a unit the container
  lacks falls through to live parse PER UNIT, not per container
  (if false, the synthetic prelude must be provably total — enumerate
  the auto-include table and assert coverage at freeze time);
  (b) freeze content — confirm a dialect TU's freeze carries only the
  units it should (system/embedded headers) and what user-unit
  filtering exists; (c) arena-enabled parse cost on the prelude TU;
  (d) confirm the config-gate fields cover binary identity (a rebuilt
  dev binary must not bind yesterday's container).
- **S1 — the prelude freeze-and-bind in `project_parse_all`** behind
  the precedence rule (only when the normal probe found nothing), no
  cross-run cache yet (freeze every run). Gate: testproject* family,
  testprojectinit*, testprojectvalue*, the 94-log parity gate
  byte-identical, and the timing table re-measured.
- **S2 — the cross-run cache** (config-gated reuse, atomic rename,
  scratch location). Gate: same + a stale-container negative control
  (corrupt/mismatched container must MISS loudly into a re-freeze,
  never bind).
- **S3 — measurements banked**: the table above re-run on both
  binaries; the A10 gate wall time recorded in docs/test-status.md.
  Perf target: gate ≤ 2.5 min on the dev binary; game invocation ≤ 1.2s.

## Interactions / gotchas (stated up front)

- **Pack-degradation gate (#63)** concerns the RELEASE pack's
  profiles; this scratch container never enters that lane. State in
  the freeze diag (DBG-gated, marker discipline) which container a TU
  bound, so a surprise bind is diagnosable.
- **The headerless lane** masks nothing new: it runs the packed
  binary with include roots masked; the prelude container is a dev-lane
  artifact and must be ABSENT there (headerless negative control
  unaffected — verify in S2's negative control).
- **Testing surface**: the ~1100 single-file tests keep live-parsing
  every embedded header on every fulltest run; only the project lane
  binds the prelude. The gate keeps exercising the full game logic
  live; only the identical header prefix is bound.
- The freeze lane's ordering rule holds: the freeze runs in its OWN
  MIR bracket BEFORE the project's bracket opens (project_parse_all
  is pre-bracket by design — same rule that keeps throws outside).

## Cost/benefit

One-time: S0 recon + ~a day of engine work + gates. Recurring win:
–7 min per merge-wave battery, dev game runs 5x faster, and every
future madc-dialect multi-TU project (the SMAUG-scale dialect end
state) inherits the fix.
