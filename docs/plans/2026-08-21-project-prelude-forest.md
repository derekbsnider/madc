# JIT launch — the Python-contender plan

**Status: PLAN — awaiting owner approval. Nothing here is implemented.**

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
- **format's return type is the one real design decision.** Today
  dialect code writes `format(...).c_str()` (24 game sites) because
  format is declared returning std::string. Options for dialect mode:
  (a) ring-lifetime `const char *` (the pre-L3 carrier convention;
  add a `.c_str()` identity on char* so existing spellings keep
  working during the sweep), or (b) a `value` return — the value-first
  end state, but it lands with L3 (value by-value returns). Owner
  picks; (a) is available now, (b) is the destination.
- Expected effect: a dialect TU binds a HANDFUL of units instead of
  145; the 66 MB decode disappears; the per-TU forest cost drops to
  noise even before Leg 1 — and the dev binary's live-parse cost
  shrinks by the same closure. This is the highest-leverage slice and
  it enforces the design instead of optimizing around it.

### Leg 1 — cold launch: bind the forest ONCE per process

- **S1. One shared bound forest across the project's TU Programs.**
  The container is immutable, read-only-mmap'd state; today each
  Program owns a private CirFrozenForest with private decoded frames
  and private materialization. Share the expensive read-only layers
  (mapping, decoded frames, unit records) engine-wide; what is
  genuinely per-Program (registration into that Program's
  namespace/decl tables) stays per-Program. Thread-safety contract:
  read-only after open, same as the embedded blob today; TU compiles
  stay sequential. Expected: 11-TU cold 1.5s → ~0.5s.
- **S2. The dev binary joins the same shape** (the original draft):
  when no container binds, live-parse the prelude ONCE into a scratch
  container (synthetic prelude TU generated from the auto-include
  table; `tmp/prelude-<config-hash>.forest`; existing freeze + probe
  arm 0; producer-config exact-match is the invalidation). Expected:
  dev 5.2s → the packed number, and it inherits S1's sharing.
- **S3. Shrink the single restore itself** (helps every launch,
  single-file included — this is the R4 startup arc): decode ONLY the
  frames backing bound units (66 MB for 145 units is the tell), bulk
  decl-index import instead of per-entry registration. Target: per-TU
  forest cost ≤ 20 ms; single-file cold launch clearly UNDER Python's
  0.1s; 11-TU cold ≈ 0.2–0.3s.

### Leg 2 — warm launch: a transparent program cache (.pyc, but native)

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
wave. The A10 gate's 8m23s collapses with S2 (it runs the dev binary).

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
