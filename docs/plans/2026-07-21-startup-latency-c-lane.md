# Minimum startup latency — make the packed forest bind pay-as-you-go

**Status:** PLAN (2026-07-21). Owner-reported regression: trivial-C startup
used to be under 100 ms; C++ hello-world used to be ~650 ms. Measured today
(madc-devbox-class container, medians of repeated runs, `bin/madc-release`
v0.36.0 forest-packed):

| Probe | bind ON (default) | `--no-forest-bind` | dev `bin/madc` (-O0, unpacked) |
|---|---:|---:|---:|
| `int main(){return 0;}` (no includes) | 18 ms | — | 29 ms |
| C hello (`stdio.h` + `puts`), default dialect | **104 ms** | **29 ms** | 82 ms |
| C hello, `--std=c17` | 55 ms | 24 ms | — |
| C++ hello (`iostream` + `cout`) | **226 ms** | 416 ms | 1,310 ms |

Reproduction: the probe files are one-liner hello-worlds (see "Measurement
protocol" below); nothing in the suite is needed.

## Diagnosis

The C++ startup win (650 → 226 ms) came from the forest-packed release binary:
frozen embedded-header units bind instead of live-parsing. But the bind is
**eager and disproportionate**: a TU that includes only `stdio.h` pays ~75 ms
of bind work it doesn't need (104 vs 29 ms), and in default (`STD_MADC`)
dialect it binds a larger madc-mode unit set than `--std=c17` does (104 vs
55 ms). The C++ case earns the bind back (~165 ms saved); the C case is pure
tax. That is exactly the owner's observation: C++ improvements paid for by the
C lane.

Two further attributions the numbers settle:

- **Parse-time C++ machinery is NOT the main cost.** With the bind disabled,
  default dialect vs `--std=c17` on C hello is 29 vs 24 ms — the lookahead
  audit's context-gating largely works; remaining default-mode overhead on a
  62 KB pure-C file is ~13% (0.401 vs 0.354 s). Worth continuing, but it is a
  second-order lever.
- **The engine floor is healthy**: 18 ms parse+JIT+run with no includes, and
  madc-release compiles+runs a 62 KB / 400-function C file in 0.35 s (gcc -O0
  takes 0.47 s to compile it).

Related known gap (claude_status `known_pre_existing_gaps`): real system-header
PCH regeneration is deferred, so regenerated blobs duplicate transitive
typedefs/declarations — the frozen units a trivial C include drags in are
C++-header-sized.

## Goals

1. Trivial-C startup on the packed release binary ≤ 40 ms (i.e. within ~10 ms
   of the `--no-forest-bind` floor), default dialect, no flags.
2. No C++ regression: iostream hello stays ≤ 226 ms (should improve — a
   smaller bound set is less to decode).
3. Bind cost proportional to the include set actually used, not to the packed
   forest size.
4. Keep the bind==no-cache equivalence gate (`forest_bind_gate`) and the
   packed suite as arbiter — this is a performance change, not a semantic one.

## Non-goals

- Removing the forest/packed machinery or adding a second header path
  (`no-parallel-implementations`: live parse and bind are the two existing
  modes; this plan changes *when* bind work happens, it does not add a third).
- The full PCH include-guard/macro-state fix (tracked separately; R3 below
  only takes the de-duplication slice if the rung needs it).
- Runtime (generated-code) performance — untouched here; MIR gen level
  default stays `-O1` (`madc_opt_level`).

## Rungs

**R0 — instrument the bind.** Add a `--show-stats` breakdown of startup: blob
map, zstd segment decode, record decode, unit bind, per-unit name + cost.
Answer precisely where the 75 ms goes (decompress vs decode vs bind) and how
many of the 240 packed units a `stdio.h`-only TU touches today. No behavior
change. Everything later cites these counters.

**R1 — demand-driven unit bind.** Key the bind off include resolution: when
`#include <stdio.h>` resolves to an embedded header, bind that unit (and its
recorded dependencies) at that moment — nothing else, nothing eager at
startup. The C hello case should bind O(1) units; the iostream case binds the
same closure it effectively uses today. Segment decompression must follow the
same demand (zstd-decode only segments containing needed units — the
per-segment compression from #37 already gives the boundaries). Gate: the
bind==no-cache equivalence check unchanged; counters show trivial-C binds a
handful of units.

**R2 — std-aware unit selection.** The default-dialect penalty (104 vs 55 ms
bound; larger unit set in madc mode) means unit choice is keyed by
`LanguageStd`. After R1 this should mostly dissolve (only included headers
bind), but verify with counters that a `--std=c17` TU and a default-dialect TU
including the same C header bind comparably sized closures; if madc-mode
units are still fat, take the transitive-duplicate slice of the deferred PCH
regeneration for the C-header units only.

**R3 — (only if R1+R2 miss the ≤ 40 ms gate) crossover check.** If residual
fixed cost remains (e.g. blob directory decode), consider deferring even the
directory work until the first embedded include resolves. Prefer shrinking
the fixed cost over adding a bind-vs-parse heuristic — a heuristic is two
paths again.

**R4 — context-gating continuation (independent, minor).** The ~13% default-
dialect overhead on larger pure-C TUs: continue the 2026-06-23 lookahead-audit
rule (context-gate at call site, C-mode gate in the deepest shared helper) on
the next callgrind ranking. Separate commits; not gated on R1–R3.

## Measurement protocol (repeat per rung)

- Probes: `int main(){return 0;}`; `#include <stdio.h>` + `puts` hello;
  same with `--std=c17`; `#include <iostream>` + `cout` hello. Scratch files
  in `tmp/` (gitignored), 5 runs, report median `real`.
- Matrix: `bin/madc-release` with and without `--no-forest-bind`.
- Record results in this file per rung. Baselines above are host-specific;
  re-measure the full matrix on the owner's box before declaring the gate met.
- Validation per change: `make -C src fulltest` (packed arbiter + bind gates
  included); `bash scripts/run_tests.sh --exe` if any shared codegen surface
  is touched (not expected — this is startup/bind machinery only).

## R0 results (2026-07-22, feature/startup-latency-claude)

Instrumentation landed: `--show-stats` now prints a forest section — map+open /
bind / restore (decl-index sweep + materialize + register) / unit loads /
decode traffic (zstd frames, bytes, seconds) / per-unit bind self-costs.
Counters live on `snapshot_reader` (decode), `CirFrozenForest` (open, unit
loads, materialize), and `Program` (map/open/bind/restore/declidx + per-unit),
flattened through `Program::forest_bind_stats()`.

Timing matrix at HEAD (5-run medians, this host, packed `bin/madc-release`):

| Probe | bind ON | `--no-forest-bind` |
|---|---:|---:|
| ret0 (no includes) | 6 ms | — |
| C hello (default) | **94 ms** | **16 ms** |
| C hello `--std=c17` | 45 ms | 15 ms |
| C++ hello | 203 ms | 396 ms |

Counter breakdown (single `--show-stats` runs):

| Counter | C hello | C++ hello |
|---|---:|---:|
| forest open | 30 ms | 31 ms |
| forest bind (include walks) | ~0 ms (26 units / 240) | 1 ms (184 units) |
| forest restore | 38 ms | 73 ms |
| — decl-index sweep | 4 ms | 5 ms |
| — arena materialize | 24 ms | 28 ms |
| — register | 10 ms | 40 ms |
| node-record segments loaded | 0 | 4 |
| zstd decode (whole process) | 19 ms, 241 frames, 14.9 MiB | 20 ms, 533 frames, 15.4 MiB |

**Where the 78 ms C-lane tax actually goes — three findings:**

1. **The per-include unit bind is already demand-driven and free** (~0 ms,
   26 units). R1's "key the bind off include resolution" is the shipped
   architecture; the eager cost is elsewhere.
2. **`open()` is a fixed 30 ms** regardless of what the TU includes: the
   intern-spine + arena zstd decode (the #37 compressed spine) plus the
   eager whole-arena name indexes (`_extern_by_name` map over every extern
   record, `_type_names` over every arena def, `_unit_by_name`).
3. **`forest_restore_decls` is whole-container, not closure-proportional**:
   the recordability fixpoint in `materialize_from_arena` walks every arena
   aggregate (24 ms) before the rung-2a filter trims what actually
   materializes, and the decl-index demand sweep decodes all 240 units'
   index segments (4 ms, 240 of the 241 frames).

**Bonus finding — the `--std=c17` anomaly explained:** the v27
producer-config gate (language_std/defines_hash) is checked AFTER `open()`
completes, so a config-mismatched compile pays the full 30 ms open and then
live-parses. The config words live in the directory header — a header-only
peek before the heavy binds makes rejection ~free. (45 → ~15 ms.)

**R1 scope, revised by the counters:** (a) config-gate before the heavy
open; (b) defer the open-time name indexes + intern/arena decode to first
actual bind; (c) make materialize + register closure-proportional (fixpoint
over the demand-filtered reachable set, not the whole arena). The include-
keyed unit bind itself needs no change.

## Success criteria

- C hello (default dialect, packed release, no flags): ≤ 40 ms on this host.
- C++ iostream hello: ≤ 226 ms (no regression).
- `--show-stats` bind counters demonstrate proportionality (trivial C binds
  a handful of units, not the forest).
- fulltest + packed arbiter green; forest_bind_gate 18/18 unchanged;
  bind == no-cache equivalence intact.
