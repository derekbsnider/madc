# JIT launch — the Python-contender plan

**Status: APPROVED (owner, 2026-08-21) — LEG 0 LANDED the same day
(@dfa6668e..@417a80bc + the dialect-lean rule/gate @d9cd5306). Measured:
the probe TU went 0.17s/138-units/66.5MB → 0.016s/0-units/11.6MB; the
11-TU game 1.5s → 0.85–0.90s. LEG 0b LANDED session 115
(@93e7fcb8..@cb42d6d7): all 57 guarded-only polyglot publics now have
lean primaries (php 18, perl 11, python 7, ruby 6, js 5, rust 10) —
six pure-dialect tests (test{php,perl,py,ruby,js,rust}lean.mad), the
94-log parity gate byte-identical, check-dialect-lean green. En route:
frozen-carrier mutation defect fixed (29 direct value::array() sites
would throw across the extern-C boundary into JIT frames — now routed
through value_array_for_write/_reset_for_write with the
test_ns_frozen.cpp reducer), ns_common gained the shared lean plumbing
(value_text_slot / ring_apply / value_pop_element / value_shift_element).
The no-viable-overload FALLBACK defect is also FIXED (@cfcd255e —
loud "no matching overload" error + the two ranker coercion gaps the
fallback had been papering over; full suite 1131/0).
S1 LANDED @21c57941 (2026-08-22, game 829→760 ms). S2 and S3 CLOSED
without code the same day — their premises dissolved under
measurement (see "S2/S3 findings" below). S5 LANDED @f13938f9 the
same day: **warm 11-TU launch 92 ms packed (176 ms dev) — under the
Python port's ~0.10 s — with the 94-log corpus byte-identical from
cache. THE PLAN'S LEGS ARE COMPLETE.** Residue: the ≤50 ms warm
stretch, the S4 interop cached-thaw 0.33 s, and the queued small
fixes (--show-stats silent under project mode and --run-frozen;
--project --emit=c11 dup __madc_global_init).**

**Owner rulings (2026-08-21, during Leg 0 — codified as
`.claude/rules/dialect-lean.md` + `scripts/check-dialect-lean.sh` in
fulltest): (1) "as far as --std=madc goes, we try to avoid depending on
C++ system header parsing unless we can reduce the overhead
significantly... until such time... we lean more heavily on madc
builtins, and polyglot conveniences (php, python, etc)"; (2) "nothing
the madc language defines within its own dialect or the polyglot
functionality should depend on std::string (beyond potential internal
to the madc source code usage)." Ruling (2) also drove the carrier
element SLOT model (@dfa6668e): `arr[i]` is a value lvalue over
madarray_index_slot — which fixed a pre-existing SILENT bug (the
string-first element model dropped `arr[i] = x` writes in a hidden
std::string temp, exit 0) — and python::format now rides the ONE
std::format engine with a value-out lean primary (@417a80bc).**

**Owner priority (2026-08-21, verbatim intent): "I need for madc to be a
JIT-language contender for Python... the fact that we can also do AOT
is a secondary bonus."** The JIT launch is the product; AOT stays the
bonus. This plan replaces the earlier draft of this file (which only
attacked the dev binary's live parse) — the packed-binary measurements
below showed the true shape.

## The measurements (2026-08-21, container; adventure = 11 dialect TUs)

| launch                                             | wall  |
|----------------------------------------------------|------:|
| Python port (advent.py: interp + source + DB)      | ~0.10s |
| madc packed JIT, single tiny file                  | ~0.10s |
| madc packed JIT, ONE 1644-line TU (--show-stats)   | 0.18s |
| madc packed JIT, the 11-TU project                 | ~1.5s |
| madc dev (unpacked) JIT, the 11-TU project         | ~5.2s |
| madc `--run-frozen` thaw of one frozen TU          | ~0.8s |
| madc AOT binary of the whole game (world+run)      | 0.007s |
| gcc -O0 compile+link of the emitted C11            | 0.42s |

The 0.18s single-TU split (packed, `--show-stats`): user-code compile
is ~0.04s (lex 0.031 + parse 0.007 + cir 0.039 + c2mir 0.006, part
forest-carved); **~0.12s is FOREST CONTAINER work — map+open 0.011,
restore 0.049 (decl-index sweep + materialize + register), unit loads
0.024, zstd decode 0.036 (797 frames → 66 MB decompressed to bind 145
of 339 units)**. Eleven TUs repeat that container work eleven times —
~730 MB of decompression per game launch. That ≈ the whole 1.5s.

So THREE compounding causes, one root — per-Program header state with
no process-level sharing and no laziness:
1. **Packed, multi-TU**: the same immutable container is re-mapped,
   re-decoded, re-materialized, re-registered PER TU.
2. **Unpacked (dev), any dialect TU**: no container at all → full live
   parse of the embedded headers per TU (the original draft's target).
3. **Warm launches**: nothing is cached between runs — every `madc
   game` recompiles everything; `--freeze`/`--run-frozen` exists but
   thaw measures ~0.8s for one TU and still live-parses real headers
   (a c++locale.h diagnostic fires during thaw) — it is not load-only
   today, so it is not yet the warm-launch answer.

Python's counterpart numbers: interpreter ~0.01s, and it CACHES
compiled bytecode (.pyc) so warm launches never re-compile. To be a
contender the JIT needs the same two properties: a cheap cold start
and a warm start that loads instead of compiling.

## The plan — eliminate first, then share, then cache

### Leg 0 — the dialect prelude must not pull the C++ headers AT ALL

**Owner ruling (2026-08-21): "is any of this game depending on C++
system header parsing/usage? this should _not_ be the case... that's
why we're not using std::string, iostream, fstream, set/map/vector/etc
within the game code."** Measured root: the GAME code is clean, but
the prelude is not — `bits/std_format` does `#include <string>` for
exactly ONE declaration (`std::string format(...)`'s return type), and
`ns_php` / `ns_madc` include/require it for their std::string-flavored
C++-INTEROP overloads (40 / 25 mentions) that dialect code never
calls. That single `<string>` closure is the 145-unit / 66 MB bind.

- **S-lean: split every dialect-serving fragment into two layers.**
  A DIALECT-LEAN fragment — value& / const char* / long declarations
  ONLY, ZERO includes — served by the auto-include scan under
  --std=madc; the full C++-interop fragment (std::string overloads,
  `#include <string>`) stays what `#include <ns_php>` means in C/C++
  modes and for embedding hosts. One publics list, two renderings —
  never two hand-maintained copies (generate the lean layer or gate
  lines by mode within one file; no drift).
- **format's return type — SETTLED (owner, 2026-08-21).** Dialect
  `format()` returns ring-lifetime `const char *` (the pre-L3 carrier
  convention); the declaration flips to a `value` return when L3
  (value by-value returns) lands. The game's `format(...).c_str()`
  sites are ~7 (the earlier "24" was a miscount; the other ~92
  `.c_str()` sites are var-carrier ring reads, unaffected) — they
  drop `.c_str()` in the same sweep. NO `.c_str()` identity on
  `char*`: a stale `format(...).c_str()` spelling must be a loud
  compile error, not a silently-tolerated wart (owner ruling).
- Expected effect: a dialect TU binds a HANDFUL of units instead of
  145; the 66 MB decode disappears; the per-TU forest cost drops to
  noise even before Leg 1 — and the dev binary's live-parse cost
  shrinks by the same closure. This is the highest-leverage slice and
  it enforces the design instead of optimizing around it.
- **LANDED + MEASURED (2026-08-21, container, packed):** probe TU
  (php+format profile) 0.164–0.182s → **0.016s**, 138/339 units bound →
  **0**, 66.5 MB decoded → 11.6 MB (14 decl-index frames — the S3
  lazy-decode target); the 11-TU game 1.46–1.59s → **0.85–0.90s**. The
  dominant residue is now user-code compile (~0.04s/TU) + the per-TU
  container open/decode (~20 ms × 11) — precisely S1/S3 — and the warm
  launch (S5) still recompiles everything.
- **Leg 0b (LANDED session 115, @93e7fcb8..@cb42d6d7 — mandated by
  owner ruling #2):** every polyglot public has a lean PRIMARY form.
  The 57 guarded-only functions (php 18, perl 11, python 7, ruby 6,
  js 5, rust 10) gained value/char* primaries with SOURCE-LANGUAGE
  parity semantics of record: PHP/Python/Ruby(non-bang)/JS/Rust text
  functions return a NEW ring-lifetime string and never mutate the
  subject (the guarded std::string& in-place forms are documented as
  C++-interop conveniences); perl::chop/chomp MUTATE the value —
  that IS Perl; element returns (php::array_pop/shift/get,
  perl::pop/shift, rust::first/last/get/pop) use the value out-param
  shape (null when empty). One algorithm per concern: the lean forms
  run the SAME in-place cores via ns_common::ring_apply /
  value_text_slot; element moves via value_pop_element /
  value_shift_element. Six pure-dialect tests pin every public + the
  non-mutation contracts; docs/language/ns-*.md updated. En-route
  found-defect fixes (own commits): the frozen-carrier mutation abort
  class (@93e7fcb8, reducer tests/unit/test_ns_frozen.cpp) and
  php::intval's unbounded payload read (@eae431ae).
  The no-viable-overload FALLBACK defect is FIXED (@cfcd255e): a
  ranked strict set with no viable candidate is a loud
  "no matching overload" compile error instead of a silent wrong-ABI
  bind; the suite fallout pass exposed (and fixed) the two coercions
  the fallback had been carrying — std::string→char* argument
  conversion (a marshals_value_text UDC arm in score_arg_to_param,
  mirroring build_call_args' object_cstr_arg lowering) and
  partial-subscript row decay of multi-dim fixed arrays (a
  TokenSubscript arm in array_decay_pointer). Full JIT suite 1131/0 +
  the 94-log parity gate green after.

### Leg 1 — cold launch: bind the forest ONCE per process

**S1 LANDED (@21c57941, 2026-08-22):** two process-level caches at the
forest layer — cir_forest_map_image maps each carrier file ONCE (the
mappings were already never munmap'd; per-TU re-maps also gave the same
blob different base addresses), and forest_pool_block binds compressed
container-global segments from a decoded-segment cache keyed (blob
base, segment offset). Evidence: MADC_FOREST_CACHE_PROBE on the 11-TU
game went 99 miss/0 hit → 9 miss/90 hit; interleaved A/B ×8: launch
829 → **760 ms** median. Gates: parity 3+94 byte-identical, project/
freeze subset, PACKED suite 1131/0. Materialization stays per-Program
by design (the class audit: _mat_storage/_restored* are Program-bound;
sharing DataDef objects across Programs is a correctness risk). Residue
for a later slice: unit-record segments (read_unit_seg) and the
template payload still decode per Program — cache only after auditing
their consumers for post-decode mutation.

- **S1. One shared bound forest across the project's TU Programs.**
  The container is immutable, read-only-mmap'd state; today each
  Program owns a private CirFrozenForest with private decoded frames
  and private materialization. Share the expensive read-only layers
  (mapping, decoded frames, unit records) engine-wide; what is
  genuinely per-Program (registration into that Program's
  namespace/decl tables) stays per-Program. Thread-safety contract:
  read-only after open, same as the embedded blob today; TU compiles
  stay sequential. Expected: 11-TU cold 1.5s → ~0.5s.
- **S2. CLOSED 2026-08-22 without code** — the premise (dev live-parse
  of the prelude worth sharing) dissolved with Leg 0: the lean prelude
  live-parse costs less than binding a container would (12 ms dev vs
  16 ms packed, whole launch). See "S2/S3 findings" below.
- **S3. CLOSED 2026-08-22 without code** — its targets were already
  met by Leg 0 + S1 (first-TU forest cost 17 ms, ~0 after; single-file
  cold 16 ms). The lazy-decode and raw-spine mechanisms stay banked
  for interop/headerless lanes. See "S2/S3 findings" below.

### Leg 2 — warm launch: a transparent program cache (.pyc, but native)

**S5 LANDED (@f13938f9 + gate @4054b4de, 2026-08-22, session 116).**
Implemented exactly per the S5 design section below. Measured
(container, `echo quit |`, output byte-identical to the live run in
every lane): **packed warm 754 → 92 ms — UNDER the Python port's
~0.10 s, with native JIT code**; dev warm 1914 → 176 ms; cold (first
launch, pays parse + freeze) packed 1.35 s / dev 3.6 s. Evidence:
per-TU staleness verified (an edited adv_score.mad refroze alone —
ten containers kept their mtimes); the **warm-cache adventure parity
replay is byte-identical across all 3 fragments + 94 whole logs**;
`scripts/check-program-cache.sh` (in fulltest) gates cold/warm
equality, the MIR fast path engaging, staleness biting (negative
control), corruption self-heal, and both kill switches. The suite
runners + adventure_parity.sh export MADC_NO_PROGRAM_CACHE=1 so
existing project tests keep testing the live compile. En route: the
S1 mapping cache gained cir_forest_map_invalidate (a refreeze's
atomic rename is the one in-process writer that breaks its
path→content assumption). Residue for later: warm 92 ms vs the ≤50 ms
stretch target — the remaining per-TU cost is container open/decode +
MIR_read + link, profile before squeezing.

- **S4. Make thaw load-only.** Find why --run-frozen re-parses real
  headers and costs 0.8s for one TU (the c++locale.h diagnostic is the
  thread to pull); a thaw must touch source zero times (LOADED ==
  parsed law). MIR cache: madc_cir_freeze already carries a mir_cache
  flag — a frozen program that also carries its MIR skips c2mir AND
  codegen at launch.
- **S5. Project-aware, automatic caching.** `madc advent.cc.json`
  transparently maintains a frozen container per manifest (keyed:
  source content hashes + config + binary identity — the same
  exact-match gate; stale = recompile + refreeze, never a wrong run).
  First launch pays the compile; every later launch thaws. Target:
  warm 11-TU launch ≤ 50 ms — under Python's floor, with native code.
- Explicit flags (`--freeze-run`, `--run-frozen`) stay; S5 only makes
  the default path do what .pyc does without being asked.

### Sequencing and gates

S0 (recon, no code): confirm which forest layers are Program-free
(decoded frames, unit records) vs Program-bound (DefArena
materialization, registration); the exact accounting of the 0.18s
(the stats buckets overlap); why thaw live-parses. Then S1 → S2 → S3
(leg 1 lands as a wave), then S4 → S5. Every slice: the 94-log parity
gate byte-identical + testproject*/testprojectinit*/testnsdmiglobal +
the timing table re-measured and recorded. fulltest once per merge
wave. (The A10 gate's 8m23s already collapsed with Leg 0 — the dev
binary's per-launch prelude cost is now 12 ms, so S2 had nothing left
to collapse.)

### S2/S3 findings (2026-08-22, session 116 — measured at HEAD, container)

**S2's premise dissolved with Leg 0 — slice CLOSED, no code.** Measured
(dev bin/madc, no container anywhere — stats confirm "no container —
live parse"; no sidecar/env/ini): a single lean TU launches in **12 ms
on the dev binary vs 16 ms packed** — the lean prelude live-parse is now
CHEAPER than binding the container. The scratch prelude container would
share a ≤12 ms cost and pay ~16 ms to do it. The 11-TU game on dev is
1.91 s vs packed 0.75 s, but the delta is the -O0 ENGINE compiling user
code (per-TU fixed cost ×11 ≈ 130 ms of it only) — a prelude container
cannot touch that, and user-code compile throughput is this plan's
explicit non-goal (front-end performance arc). The A10-gate collapse S2
promised already happened via Leg 0 (the gate's dev launches carry the
12 ms prelude, not the old <string> closure). A scratch container for
C++-INTEROP dev compiles remains the "dev sidecar pack" NON-GOAL (it
would flip live-parse testing to bind testing — separate owner lever).

**S3's targets are ALREADY MET by Leg 0 + S1 — slice CLOSED, no code.**
Targets vs measured: per-TU forest cost ≤20 ms → 17 ms first TU
(map+open 9 + decode 8), ~0 for TUs 2..N under S1; single-file cold
clearly under Python's 0.1 s → 16 ms packed / 12 ms dev; 11-TU cold
0.2–0.3 s → 0.75 s, but the residue is user-code compile (~0.44 s
at -O2), not forest cost — same non-goal. The two S3 mechanisms stay
banked for the interop/headerless lanes if their numbers ever warrant
them: lazy per-segment decode on first query (extern-locs precedent)
and raw-spine storage (a pack-size trade = owner sizing call).

**Leg 1 is therefore COMPLETE (S1); Leg 2 = S5 is the remaining real
slice** (S4 already met for lean TUs — 6 ms thaw; its interop residue
is the 0.33 s MIR_read/link of the drained module).

### S0 findings (2026-08-21, session 115 — measured on the fresh release binary)

- **Post-Leg-0b numbers hold at HEAD:** lean probe TU 15–16 ms; the
  11-TU game 0.81–0.84 s.
- **The 14-frame / 11.6 MB decode at ZERO units bound** (probe stats:
  map+open 0.009 s, decode 0.008 s — essentially the whole forest
  cost) is complete_open decoding the ALWAYS-BOUND container globals,
  per Program: the release pack compresses the intern spines
  (container pool + arena pool — task #37: 3.74→0.81 MB stored) plus
  arena defs/payload/tokbytes and the PP surfaces, and
  forest_pool_block's raw_ptr returns NULL for a compressed segment,
  so each open pays the owned-buffer zstd decode. S3 levers, in
  preference order: S1's process-level sharing makes it once per
  LAUNCH free of charge; true lazy per-segment decode on first query
  (the extern-locs segment already works this way); storing the
  always-touched spine raw is a pack-size trade (owner sizing call).
- **S1 feasibility confirmed:** CirFrozenForest state is Program-FREE
  (mmap, directory, decoded owned buffers, read-only arena bindings —
  the code's own comment: DataDefs "materialize lazily at bind ...
  never at open", into the PROGRAM's structures). Per-Program residue
  on the object is only the _work_secs/_work_depth stat pointers
  (trivially movable). Choke point: ensure_bind_forest /
  probe_forest_chain — a process-level cache keyed (image identity,
  producer config, defines hash) hands out one shared instance.
  Thread contract to state: read-only after complete_open; the
  memoized lazy decodes (ensure_template_payload, extern-loc map)
  fill once; TU compiles are sequential today.
- **S4 PREMISE CORRECTED — thaw does NOT live-parse.** The
  c++locale.h diagnostic that drove the "live-parses real headers"
  claim is a c2mir WARNING carrying a thawed node's file:line (the
  MC11-IR position, by design); it disappears under the MIR cache,
  which skips c2mir entirely. Measured: LEAN TU thaw+run 8 ms plain,
  **6 ms with --freeze-mir-cache** (node rebuild skipped) — load-only
  already holds, and the warm-launch target (≤50 ms) is ALREADY MET
  for lean dialect TUs by existing machinery. INTEROP TU (the
  <string> closure): 0.77–0.83 s plain = c2mir + codegen over the
  drained library closure (not parsing); **0.33 s with the MIR
  cache** (+150 KB container) = MIR_read/link of the big drained
  module — an interop-only residue for a later slice.
- **S5 reframes to plumbing:** transparent per-manifest freeze
  (content-hash keyed, refreeze on mismatch) + project-mode
  integration of the MIR cache. Projected warm 11-TU launch ≈ 11 ×
  ~6 ms + link — well under Python's 0.10 s floor.
- Queued small fix joins the list: --show-stats is silent under
  --run-frozen (same family as project mode ignoring it).

### S5 design (2026-08-22, session 116 — recon-verified against the code)

**Shape: per-TU cache containers + ONE run path.** `madc <manifest>`
maintains `<manifest-dir>/__madcache__/<tu-stem>-<key8>.forest` — one
standard standalone container per TU (the `--freeze` format; N files,
not a stacked one, so each is independently inspectable with
`--dump-forest` and one edited TU refreezes ALONE — per-TU .pyc
semantics). The project JIT lane becomes: probe each TU's cache →
MISS: parse + freeze it (then thaw what was just written — the
--freeze-run precedent, in-process) → thaw every TU's module into the
ONE shared MIR context → link → per-TU inits → entry. Warm and cold
launches share the thaw path, so it is exercised on every run, not
only warm ones.

- **Key (the "never a wrong run" gate), validated in two layers:**
  filename key8 = fnv64(TU path | producer config word | defines hash
  | context pin | self-exe mtime+size — the BINARY-IDENTITY term the
  pin alone lacks: the pin folds only VERSION+format, and a dev
  rebuild must invalidate); inside the container a NEW small segment
  `SRC_STAMPS` records (path, content-hash) for the TU + every USER
  source file the parse read from disk (embedded/forest sources are
  covered by pin+config). Thaw re-hashes every stamped file; any
  mismatch = MISS = refreeze. Manifest edits need no extra key: per-TU
  flags live in the config word/defines hash, entry + TU list are
  read live at thaw.
- **Freeze side:** `madc_cir_freeze` gains the project_tu shape
  (threaded to CirBuilder) so each frozen TU takes the TU-unique
  STATIC init (`tu_init_symbol(tu.file)` — deterministic from the
  path, so the thaw side RECOMPUTES it; no format field needed) and
  main carries no init prologue — the engine keeps its ld.so role.
  No pack_recording (no groves, no drain gate — the cheap
  --freeze-run shape) + `--freeze-mir-cache` semantics always on.
  forest_arena_enabled set before tokenize for miss TUs.
- **Thaw side:** per TU: map + open_header (pin) + exact config gate
  + stamp validation, then MIR_read the module blob into the shared
  ctx (fallback: materialize the frozen root and cir_compile it in
  the shared c2m — build_frozen's own fallback, shared-context
  flavor). dlopen each container's recorded libs (the flavor runtime
  closure) before link. Trap prebinding generalizes
  cir_prebind_cache_traps to N modules: only a name NO module defines
  and the resolver cannot find is trap-bound.
- **Failure policy: the cache is DERIVED state.** Any cache failure
  (unwritable dir, freeze failure, corrupt container) falls back to
  the live compile for that TU — never fails the run (the MIR-cache
  precedent); diagnostics DBG-gated. Writes are mkstemp +
  atomic-rename (concurrent runs safe; readers keep the old inode).
- **Transparency + kill switch:** default ON for the `--project` JIT
  lane only (AOT lanes unchanged). `MADC_NO_PROGRAM_CACHE=1` (env)
  and `--no-program-cache` (flag) disable. **The suite runners and
  adventure_parity.sh export the env kill-switch** so existing
  project tests keep testing LIVE compile (the dev-sidecar-pack
  lesson — a cache must not silently flip the suite from live-parse
  testing to thaw testing); dedicated `testprojectcache*` fixtures +
  an explicit warm parity leg exercise the cache lanes.
- **Cost model:** miss TU ≈ parse + translate (freeze) + thaw-compile
  (MIR blob) + MIR_read (~2× today's compile, first launch only);
  hit TU ≈ open + MIR_read (~few ms). Projected warm 11-TU launch:
  11 × ~6 ms + link ≈ well under Python's 0.10 s floor.
- **Gates:** parity 3+94 byte-identical on BOTH lanes (env-off live;
  warm-cache leg), testproject*/testprojectinit*/testnsdmiglobal,
  staleness tests (edit one TU → exactly that TU refreezes; binary
  swap → full refreeze), kill-switch test, timing table re-measured.

## Non-goals / rejected

- Pointing the fulltest gate at the packed binary (tests must run the
  binary they build).
- A dev sidecar pack by default (flips the whole suite from live-parse
  testing to bind testing — separate owner lever).
- Sharing LIVE parse products across Programs without the container
  (fights the pool-activation model; the container is the designed
  vehicle — and after S1/S3 it is fast enough).
- Chasing gcc's compile throughput as the primary lever: user-code
  compile is ~0.04s/TU — real but second-order next to the container
  work; it stays on the front-end performance arc.
