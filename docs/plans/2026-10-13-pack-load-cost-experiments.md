# Pack load cost — measurements and proofs of concept

Status: PLAN (owner review). Owner 2026-10-13: "I don't want to make big
decisions without hard data to properly weigh the costs and benefits... this is
not a snap decision moment" / "we really need to execute a series of tests and
proof of concepts to know for sure".

## Why

Binding pre-lexed, pre-parsed headers should cost less than lexing and parsing
them. Today it does not for small programs: `#include <stddef.h>` takes 3.6 ms
parsed live and 27–28 ms bound from the pack (release build, warm cache, median
of 30). The frozen data is not the problem; the loading is all-or-nothing:

- every bind decompresses the pack's shared tables — the string pool (4.2 MB
  raw) and the type arena (7.3 MB raw) — though one small header needs a few
  dozen of their entries;
- the restore walks all 19,372 arena records and decodes every grove header's
  declaration list to decide what the one bound header declares;
- about 80% of the arena and 68% of the pool belong to the C++ standard
  library, which a C program never uses.

The internal dialect headers (Decision `dialect_headers_internal_indexed`) are
blocked on this: a pre-built prelude must load for less than its live parse
(~8 ms) or it is a regression.

## Rules for every test

- One test at a time, nothing else running on the build container.
- One fixed baseline: commit 6d83e4ccc, the symbolized -O2 build
  `tmp/relbuild/head-sym3` (matches the shipped binary within 0.02% Ir), the
  release pack `lib/release/libmadc.so` from the same commit.
- Each variant is a SCRATCH build or a scratch container — nothing committed
  during this phase.
- Report instructions (callgrind Ir, exact) AND wall clock (30 runs,
  interleaved A/B, median + p10/p90). Warm cache; plus one cold run where
  disk reads could matter.
- Keep each test's tool and command in `tmp/pc/` so it can be re-run.

## The workload ladder (used by every test)

| workload | what it binds |
|---|---|
| `#include <stddef.h>` | one tiny C header |
| `#include <stdio.h>` | a real glibc header and its includes |
| `#include <vector>` | one mid-size C++ header |
| a dialect-sized set (`<ns_php>`-like C++ declarations, ~1,500 lines) | stand-in for the prelude |
| `scripts/parse_cost/cxx_stl.cpp` | a large C++ set (the pack's best case) |
| `println("hi")` | nothing (the probe cost alone) |

## Tests

1. **Baseline ladder.** Live vs packed for every workload, Ir and wall, plus
   kernel time and page faults (`/usr/bin/time -v`) and one cold-cache run.
   Answers: where the pack starts to win today, and what the fixed cost is.
2. **Compressed vs read in place.** The same container written two ways: the
   shared tables zstd-compressed (as shipped) and uncompressed, bound in place.
   Needs a scratch freeze knob for the arena (the string pool already has one:
   a standalone freeze writes it raw). Answers: what decompression costs per
   bind vs the size it saves. Owner note: compression was meant to let the
   load decompress straight into binary form with no parsing or structure
   building — this test measures that path against a raw read.
3. **C / C++ layering.** A C-only container as the proxy for a layered pack
   (C tables first, uncompressed; C++ tables after the boundary, compressed).
   First result: `<stddef.h>` 27.2 ms (full, compressed) → 10.6 ms (C-only,
   compressed) → 8.5 ms (C-only, string pool raw). Repeat across the ladder,
   then a scratch layered writer to confirm the proxy. Answers: what a C
   program saves, what the boundary check needs.
4. **Compact arena.** A scratch encoder that stores each record kind with only
   its fields (a pointer type uses ~6 of the 54, a function ~12). Answers: the
   raw arena size compact, whether it can stay uncompressed, and its load cost
   vs the fixed-size records.
5. **Restore index.** A scratch per-header record index so the restore visits
   only the bound headers' records and what they reference. Answers: how much
   of the remaining fixed cost is the whole-arena walk.

6. **Per-header floor (no code).** Freeze a container from ONE header alone
   (`<stddef.h>`, `<stdio.h>`, `<limits>`, `<type_traits>`, `<vector>`, and
   cxx_stl's include set), store it uncompressed, bind it. The restore then
   sweeps and walks only that header's records — what a perfect per-header
   index would make it do. Answers: the best a per-header design can reach,
   per workload, against live (libmadc's text) and today's pack; and whether
   C headers can ever beat live.

Measuring binary (corrected 2026-10-13): a symbolized RELEASE build with the
pack built in (`tmp/relbuild/head-sym4`: `./configure`, `make release`, no-op
strip) — never a pack-less build with a pack attached by environment
(`.claude/rules/embedded-headers.md`). "Live" = `--no-forest-bind`, parsing
the header TEXT from libmadc. Tests 1–5 above ran on `head-sym3` (pack-less +
`MADC_FOREST`): their bind numbers stand; their live column read disk
(Test 1c shows libmadc's text costs +2–5%).

## Output

One table: each option's size cost, its measured gain across the ladder, and
its complexity — for the owner to weigh. No size trade and no format change is
decided before that table exists.

## Thread safety

No runtime change in this phase. Any option adopted later states its contract
in its own design (the pack is immutable and mapped; readers share it without
locks).

## Results

### Test 1 — baseline ladder (2026-10-13)

`tmp/pc/ladder/ladder.py`: the symbolized release with the release pack
(`MADC_FOREST`) in BOTH modes, as the shipped binary carries it; live =
`--no-forest-bind`. Wall = median of 30 interleaved runs; kernel time and minor
faults from `getrusage`; cold = median of 5 runs after evicting the binary,
its library and the pack from the page cache.

| workload | Ir live | Ir packed | wall live | wall packed | sys live / packed | faults live / packed | cold live / packed |
|---|---|---|---|---|---|---|---|
| `println("hi")` | 13.6M | 15.1M | 4.0 ms | 4.6 ms | 0.0 / 0.0 ms | 1,030 / 1,112 | 14.8 / 29.7 ms |
| `<stddef.h>` | 12.7M | 174.3M | 4.4 ms | 27.6 ms | 0.1 / 7.5 ms | 996 / 6,077 | 13.7 / 54.8 ms |
| `<stdio.h>` | 57.1M | 180.2M | 9.2 ms | 28.7 ms | 2.7 / 7.6 ms | 1,529 / 6,214 | 33.1 / 55.4 ms |
| `<limits>` | 90.3M | 182.6M | 13.7 ms | 29.2 ms | 3.7 / 7.3 ms | 2,685 / 6,247 | 37.8 / 56.0 ms |
| `<type_traits>` | 99.2M | 257.9M | 15.8 ms | 41.1 ms | 3.3 / 11.5 ms | 2,888 / 8,825 | 39.1 / 66.5 ms |
| `<vector>` | 538.5M | 352.1M | 77.9 ms | 59.1 ms | 19.8 / 11.7 ms | 16,370 / 11,657 | 101.7 / 85.8 ms |
| cxx_stl.cpp | 2,836.6M | 1,660.8M | 538.6 ms | 304.6 ms | 107.7 / 59.6 ms | 64,978 / 44,957 | 579.4 / 324.9 ms |

- The pack wins between `<type_traits>` (~2,100 preprocessed lines) and
  `<vector>` (~14,200).
- The fixed cost of a bind is ~160M Ir / ~23 ms warm, ~41 ms cold.
- ~7.5 ms of it is kernel time: ~5,000 extra page faults (~20 MB first touched —
  the decompressed shared tables).
- The dialect headers are not in the pack (embedded text), so `<limits>` and
  `<type_traits>` stand in for a prelude-sized bind.
- The live C++ rows (`<vector>`, cxx_stl) still open the pack fully (~9 ms,
  `--show-stats`: "container bound, 0 units bound"). Confirmed (`-v`): a
  header's `__builtin_memset` asks `forest_adopt_declared_function(memset)`;
  the function-name index says the pack declares it, the full open runs, and
  the adoption then DECLINES it ("not in an adoptable C shape: not variadic").
  The open is wasted on every live C++ compile. Fix after this phase: the
  index answers "adoptable", not just "declared" (Gap
  `adopt_probe_opens_pack_for_nonadoptable_name`).

### Test 1b — where the packed instructions go (2026-10-13)

`tmp/pc/split4/split4.sh` (callgrind, packed, same binary and pack). decode =
`snapshot_reader::decode_payload` inclusive; transform = `byte_transpose` +
`read_segment`'s self cost (the delta inversion inlines there); restore =
`Program::forest_restore_decls` minus the decode inside it (a second run with
`--toggle-collect`); rest = the remainder.

| workload | total | decode | transform | restore | rest |
|---|---|---|---|---|---|
| `println("hi")` | 15.1M | 0 | 0 | 0 | 15.1M |
| `<stddef.h>` | 174.3M | 73.4M | 0 | 60.2M | 40.7M |
| dialect.mad | 78.7M | 0 | 0 | 0 | 78.7M |
| cxx_stl.cpp | 1,660.8M | 197.7M | 0 | 156.7M | 1,306.4M |

- Transform costs nothing measurable: the reader binds the transformed record
  layout directly.
- `println("hi")` and dialect.mad bind nothing; dialect.mad's 78.7M is all the
  live parse of the embedded prelude.
- `<stddef.h>`: decode + restore are 77% of the total. Its rest (40.7M vs
  12.7M live) includes ~12.5M in `flush_forest_pending_globals` outside the
  restore: 7,726 `findVariable` calls and ~8,500 name constructions, for a
  header that declares a handful of names.

### Test 2 — compressed vs read in place (2026-10-13)

No scratch build was needed: `tmp/pc/t2/recodec.py` rewrites the SHIPPED pack's
chosen segments uncompressed (codec None, read in place) and copies every other
segment byte-for-byte. Control: a copy with nothing changed matches the shipped
container segment for segment (header, ids, metadata, payload bytes). Every
variant binds and reproduces cxx_stl.expect. `tmp/pc/t2/t2.py`: Ir, 30 runs
interleaved across the five variants, kernel time, faults, cold = median of 5.

| variant | container | `<stddef.h>` | `<stdio.h>` | `<limits>` | `<type_traits>` | `<vector>` | cxx_stl |
|---|---|---|---|---|---|---|---|
| A shipped (all zstd) | 11.6 MB | 27.4 ms / 174.3M | 28.6 / 180.2M | 29.2 / 182.6M | 41.5 / 257.9M | 59.1 / 352.1M | 305.6 / 1,660.8M |
| B strings raw | 14.8 MB | 23.9 / 139.2M | 25.5 / 145.1M | 25.9 / 147.5M | 37.3 / 222.8M | 55.8 / 317.0M | 295.0 / 1,625.8M |
| C arena raw | 18.3 MB | 23.7 / 146.7M | 25.1 / 152.5M | 25.7 / 154.9M | 37.7 / 230.3M | 56.2 / 324.5M | 294.3 / 1,633.0M |
| D both raw | 21.5 MB | 19.7 / 111.6M | 22.1 / 117.4M | 22.7 / 119.8M | 34.2 / 195.2M | 53.0 / 289.4M | 295.5 / 1,597.8M |
| E both + decl indexes raw | 21.6 MB | 19.1 / 104.9M | 21.4 / 110.8M | 21.8 / 113.2M | 33.5 / 188.6M | 51.3 / 282.7M | 294.8 / 1,591.1M |
| (live, Test 1) | — | 4.4 / 12.7M | 9.2 / 57.1M | 13.7 / 90.3M | 15.8 / 99.2M | 77.9 / 538.5M | 538.6 / 2,836.6M |

- Reading in place removes a FLAT ~8 ms / ~69M Ir per bind, for +10 MB of
  container (libmadc.so 23.9 → ~34 MB).
- `<stddef.h>` faults: 5,996 → 3,193 (the decompressed copies are gone); still
  ~2,200 more than live.
- Cold runs: no measurable difference between variants (noise ±10 ms).
- With NO decompression at all, `<stddef.h>` still costs ~92M Ir / ~15 ms over
  live: decompression is about a third of the fixed cost; the other two thirds
  are our own restore work (the all-units declaration sweep, the whole-arena
  materialize walk, the registration lookups). Test 5 targets that.

### Test 5 (first step) — build only what the bound headers declare (2026-10-13)

Scratch tree `tmp/relbuild/head-t5` = head-sym3 plus ONE env-gated line in
`forest_restore_decls`: `MADC_T5_EXACT` sets the restore filter's existing
`exact` mode (unindexed = not asked for; today unindexed "derived" entities are
built on EVERY bind). All arms on the uncompressed pack (Test 2 variant E).
`tmp/pc/t5/t5.py`.

Correctness (`t5.py check`): identical rc + stdout on all three arms for the
ladder and 40 `<stdio.h>` tests — EXCEPT cxx_stl.cpp, which fails in exact mode
(`Unidentified member '_M_emplace_hint_unique'` — a derived member
instantiation the pack holds). So a correct index must record EACH HEADER'S OWN
derived entities at freeze time, not drop them.

| workload | base Ir / wall | exact Ir / wall |
|---|---|---|
| `<stddef.h>` | 104.9M / 20.9 ms | 96.1M / 18.8 ms |
| `<stdio.h>` | 110.8M / 21.9 ms | 102.2M / 19.8 ms |
| `<limits>` | 113.2M / 22.0 ms | 104.4M / 19.9 ms |
| `<type_traits>` | 188.6M / 34.4 ms | 154.1M / 26.4 ms |
| `<vector>` | 282.7M / 53.5 ms | 185.2M / 33.0 ms |

(the head-t5 binary without the switch matches base Ir exactly.)

- Building unrequested derived entities is a large cost for C++ headers
  (`<vector>` −38%) and small for C (`<stddef.h>` −8.8M).
- What exact mode still spends on `<stddef.h>` (96M vs 12.7M live), by
  profile: the restore 55M — mostly JUDGING (the all-units declaration sweep
  and a name-verdict lookup per arena record), not building; 7,525 lookups over
  two whole-pack lists in the flush (`restored_nsbinds`, `restored_defbodies`),
  9M; copies of uncompressed segments at open, 4M; teardown at exit, ~6M.
- ESTIMATE, not measured: with a frozen per-header index (no sweep, no
  per-record judging, the flush lists filtered to bound headers), `<stddef.h>`
  would land near 20–25M Ir.

### Test 1c — live with NO headers on disk (2026-10-13)

Test 1's live rows read the headers from DISK (strace: `/usr/include/...`
opened — each header file twice); the build container has them installed. A
user machine without development headers reads the pack's per-unit TEXT
instead. `tmp/pc/headerless_run.sh` masks every include directory gcc searches
(the `scripts/headerless_suite.sh` mechanism, with its negative control) and
reruns the ladder:

| workload | live, disk | live, pack text | packed |
|---|---|---|---|
| `<stddef.h>` | 4.4 ms / 12.7M | 4.3 / 12.7M | 28.1 / 174.3M |
| `<stdio.h>` | 9.2 / 57.1M | 10.0 / 59.0M | 30.2 / 180.2M |
| `<limits>` | 13.7 / 90.3M | 14.7 / 92.6M | 31.5 / 182.9M |
| `<type_traits>` | 15.8 / 99.2M | 16.1 / 101.9M | 41.2 / 258.2M |
| `<vector>` | 77.9 / 538.5M | 82.5 / 549.7M | 60.3 / 352.5M |
| cxx_stl.cpp | 538.6 / 2,836.6M | 466.6 / 2,867.8M | 300.5 / 1,661.1M |

- Reading a header's text from the pack costs +2–5% Ir over disk; the
  conclusions of Test 1 hold on a headerless machine.
- `<stddef.h>` is madc's own embedded header (`include/madc/stddef.h`, like
  gcc's built-in include dir): neither disk nor pack.

### The C question — a C-only container, uncompressed, declared-only (2026-10-13)

Proxy for "C tables on their own": the Test 3 C-only container (24 common C
headers, 214 units) recoded uncompressed (`tmp/pc/t2/Cc_only_raw.forest`, 4.3 MB),
bound normally and in exact (declared-only) mode. `tmp/pc/c5/c5.py`; every arm's
rc + stdout match live.

| workload | live | today (whole pack) | whole pack raw, exact | C-only raw | C-only raw, exact |
|---|---|---|---|---|---|
| `<stddef.h>` | 4.6 ms / 12.7M | 28.6 / 174.3M | 18.6 / 96.1M | 8.4 / 39.0M | 8.0 / 37.9M |
| `<stdio.h>` | 9.9 / 57.1M | 29.4 / 180.2M | 20.1 / 102.2M | 9.5 / 44.4M | 9.1 / 43.4M |
| 8 everyday "C" headers | 159.1 / 1,264.6M | 70.1 / 394.3M | 41.7 / 232.4M | 25.1 / 136.8M | 24.0 / 130.1M |

- The same `<stddef.h>` bind costs 28.6 → 18.6 → 8.0 ms as the container
  shrinks: a bind's cost tracks the size of the CONTAINER, not of the header.
- Even the small C-only container does not beat live for a tiny C header
  (`<stddef.h>` 8.0 vs 4.6 ms) and only ties for `<stdio.h>` (9.1 vs 9.9 ms).
  The remaining overhead (~25M Ir over a no-include run) is the restore's
  all-units sweep and arena walk over 214 units, plus open and teardown.
- In the madc dialect (C++-based), `<stdlib.h>` and `<math.h>` resolve to
  libstdc++'s wrappers (`/usr/include/c++/13/stdlib.h` → `<cstdlib>`), as they
  do under g++. So "8 everyday C headers" is a C++ workload there: 159 ms
  live, and the pack wins it.
- Real C programs (`--std=c*`) do not bind the pack today (config mismatch):
  they always parse live, from disk or the pack's text.

### Test 6 — the per-header floor (2026-10-13)

Measuring binary: `tmp/relbuild/head-sym4` (symbolized RELEASE build, pack built
in, at 876faa746's content). Each workload's `#include` lines alone were frozen
into their own standalone container (`tmp/pc/t6/freeze_one.sh`; the release
pack's own freeze flags `--freeze-mir-cache --freeze-also-std=gnu++17
--freeze-also-std=c++17` — without the shared configs a `.cpp` consumer does
not bind and silently parses live), stored uncompressed, bound with
`--forest-bind=` (arm 0 — the built-in pack otherwise outranks `MADC_FOREST`).
`tmp/pc/t6/t6.py`: Ir + 30 interleaved runs; every arm's rc + stdout match live.

| workload | live (libmadc text) | today's pack | whole pack, raw | its own container, raw | own container size (zstd / raw) |
|---|---|---|---|---|---|
| `<stddef.h>` | 4.7 ms / 12.7M | 30.9 / 174.3M | 23.2 / 104.6M | **5.1 / 12.4M** | 24 KB / 0.10 MB |
| `<stdio.h>` | 9.9 / 58.9M | 30.5 / 180.1M | 22.8 / 110.1M | **5.8 / 18.2M** | 0.13 / 0.32 MB |
| `<limits>` | 15.0 / 92.6M | 32.7 / 182.9M | 25.0 / 112.8M | **6.3 / 21.4M** | 0.18 / 0.48 MB |
| `<type_traits>` | 15.9 / 101.9M | 42.0 / 258.2M | 34.5 / 188.2M | **7.1 / 28.7M** | 0.25 / 0.55 MB |
| `<vector>` | 80.7 / 549.9M | 64.6 / 352.4M | 54.4 / 282.4M | **15.0 / 76.7M** | 1.2 / 2.1 MB |
| 8 everyday "C" headers | 172.1 / 1,280.5M | 79.7 / 394.1M | 70.7 / 324.2M | **26.6 / 131.3M** | 1.6 / 3.6 MB |
| cxx_stl.cpp | 492.7 / 2,867.1M | 316.7 / 1,661.2M | 309.8 / 1,590.7M | **270.4 / 1,419.9M** | 4.6 / 11.3 MB |

- A bind scoped to what the program includes beats every other option on
  every workload. The fixed cost disappears: `<stddef.h>` binds at live speed,
  `<stdio.h>` 1.7x faster than live, `<vector>` 5.4x faster than live and 4.3x
  faster than today's pack.
- C benefits: with a per-header bind, C headers should NOT stay live.
- cxx_stl's remaining time is its own instantiation and code generation; the
  load share is small once scoped (270 vs 317 ms today).
- What this does not yet answer: how ONE pack gives every include set this
  floor — each header's records and strings stored as its own slice (the
  original plan: "strictly unpack a single header's AST branch on demand"),
  with references into the slices of the headers it includes, and what that
  costs in size against today's shared tables.

### Test 7a — the size of per-header slices (2026-10-13)

`tmp/pc/t7/slices.py`: each shared table of the release pack split into 369
equal slices (369 = the units that carry records) and each slice compressed on
its own at the release level (zstd 15), with and without a dictionary trained
on that table (dictionary counted). The hash-bucket tables (segs 4, 14) are
left out: slices would carry their own small indexes instead.

| table | raw | one frame (today) | 369 frames | 369 frames + dict |
|---|---|---|---|---|
| strings: bytes | 3.08 MB | 0.428 | 0.624 | 0.578 |
| strings: entries | 0.88 | 0.504 | 0.479 | 0.561 |
| arena: defs | 4.18 | 0.179 | 0.205 | 0.290 |
| arena: payload | 1.38 | 0.092 | 0.195 | 0.204 |
| arena strings: bytes | 0.83 | 0.082 | 0.137 | 0.137 |
| arena strings: entries | 0.29 | 0.154 | 0.162 | 0.182 |
| arena token bytes | 0.46 | 0.036 | 0.185 | 0.121 |
| template payload | 1.47 | 0.163 | 0.203 | 0.240 |
| template tokens | 4.90 | 0.850 | 1.191 | 1.106 |
| **total** | **17.47 MB** | **2.49 MB** | **3.38 MB** | **3.42 MB** |

- Compressing per slice costs ~+0.9 MB (+36% on these tables, ~+8% of the
  11.6 MB pack); a dictionary does not pay. Storing them raw instead costs
  +15 MB.
- An ESTIMATE: real per-header slices are not equal-sized byte runs, and
  ownership (which header a shared record belongs to) still has to be decided.

### Test 7b — compressed per-header containers keep the floor (2026-10-13)

The Test 6 single-header containers as frozen (zstd, the release settings)
against their uncompressed copies and live, same harness (`T6_ARMS`):

| workload | live | own container, raw | own container, zstd |
|---|---|---|---|
| `<stddef.h>` | 4.74 ms / 12.7M | 4.76 / 12.4M | 4.81 / 13.4M |
| `<stdio.h>` | 9.31 / 58.9M | 5.29 / 18.2M | 5.43 / 21.0M |
| `<limits>` | 14.13 / 92.6M | 5.87 / 21.4M | 6.11 / 25.2M |
| `<type_traits>` | 15.83 / 101.9M | 6.82 / 28.7M | 7.38 / 33.8M |
| `<vector>` | 79.33 / 549.9M | 15.53 / 76.7M | 16.58 / 89.0M |
| 8 everyday "C" headers | 155.26 / 1,280.5M | 23.60 / 131.3M | 25.54 / 151.2M |
| cxx_stl.cpp | 481.13 / 2,867.1M | 271.59 / 1,420.0M | 270.24 / 1,465.9M |

- Once a bind decodes only what the program includes, compression costs
  0.05–2 ms (≤ 8% of the bind). The pack can stay compressed: the +10–15 MB
  of raw tables buys almost nothing at this scale.
- Together with 7a: per-header slices, compressed, ≈ +0.9 MB of pack for the
  Test 6 floor — IF a sliced pack reaches the floor. That depends on
  ownership: which slice holds a record that several headers share or that
  one header's instantiation made from another's types. Not yet measured.
