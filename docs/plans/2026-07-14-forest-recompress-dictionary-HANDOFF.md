# HANDOFF — forest recompression: dictionary slice (task #37, toward the <10 MB binary)

**From:** Claude, 2026-07-14. **For:** next sitting (Claude post-compact or Codex).
**Branch:** `feature/forest-recompress-claude` · **HEAD:** `0ef1f682` (pushed, NOT merged to develop).
**Owner directive:** packed release binary <10 MB; raw-forest storage was never an agreed trade. BOTH surfaces stay regression-free.

## LANDED (slice 1 @efe1b52c — verified, do not redo)

Per-segment zstd per the written design (2026-07-04 data-substrate plan +
forest-default-mode-design §7). Binary **101.1 → 15.6 MB (−85%)**; blob 9.5 MB;
CIR_RECORDS 61.8 → 2.05 MB (30×). Key decisions inside:
- configure REQUIRES libzstd-dev (`--without-zstd` = loud opt-out; the silent
  HAVE_ZSTD-undefined zlib fallback caused the original regression).
- INTERN pool blocks stay codec None — zero-copy bind-in-place spine, keyed on
  `SNAP_KIND_INTERN_*` in `add_seg` (src/cir_freeze.cpp).
- Level by placement (madc_cir.cpp freeze site): appended release pack = 15,
  dev/standalone freeze = codec default 3. **Level 19 = 53.4s CPU on this
  corpus → SIGXCPU under the box's ~120s per-process kill** (broke the release
  pack AND forest_selfexe_gate until the split).
- `snapshot_writer::add_segment` + `madc_pch::compress` gained a level param
  (default preserves .madh behavior).
- libmadc.pc.in: Libs.private regenerated from real deps (+`-lzstd`, dead
  `-lasmjit` removed).
- Gates all green WITH THE BLOB PRESENT: fulltest 695/0/0/16 (self-exe gate
  green), release rc=0, packed suite 695/0/0/16.
- Honest wall cost: bound testsubscript (worst case — closure spans the corpus)
  0.57 → 0.62s (+~50ms decode); small TUs proportional (~0.29s).

## ⚠️ TRAPS (paid for this sitting)

1. **A FAILED pack leaves a blob-less binary and the packed suite silently
   passes as a LIVE-parse run** (no-magic-at-EOF → fallback). Always verify
   blob presence (binary size, or the census script) alongside packed-suite
   results. Census: `scratchpad seg_census.py <bin>` — or re-derive: footer is
   last 32 bytes (dir_offset u64, blob_size u64, seg_count u32, version u32,
   magic "MADCSNAP"); directory entries 40 B (seg_id, kind, offset, comp_size,
   raw_size u64s, codec, flags).
2. The box hard-kills processes around **120s CPU** regardless of `ulimit -t`
   in scripts. Budget pack-time compression accordingly (pack freeze alone
   ≈ 65–70s CPU at -O2).
3. `--run-frozen` proves container round-trip only — it compiles the pack's
   own defs. The packed suite + bind gates are the arbiter (family-D lesson,
   re-confirmed here).

## MEASURED (2026-07-14, corpus = tmp/_bfDEV.msnap 87 MB raw; logs in scratchpad)

Whole-file zstd (upper bound incl. cross-segment redundancy):
```
L3  6.31 MB 13.2x 0.2s | L6 5.94 0.8s | L9 5.83 1.2s | L12 5.75 2.1s
L15 5.69 MB 14.6x 4.9s | L19 4.12 MB 20.1x 53.4s  <- btultra2 depth, unaffordable
L12+LDM+w27 5.34 2.2s | L15+LDM 5.32 4.5s | L16+LDM 5.14 9.0s | L17+LDM 5.00 9.6s
```
Real per-segment blob at L15: 9.5 MB total (intern 3.74 raw + compressed rest).
The whole-file-vs-per-segment gap ≈ cross-UNIT redundancy = the trained
dictionary's headroom (240 per-kind sibling frames are highly similar).

## THE TASK — shared trained ZDICT dictionary (the design's named follow-up)

Design ref: forest-default-mode-design.md §7 ("Shared trained zstd dictionary…
per-file frames are now numerous and small; requires HAVE_ZSTD + ZDICT; the
per-segment codec field already carries the flip").

1. Train per-KIND dictionaries at pack time (`ZDICT_trainFromBuffer`) over the
   240 sibling frames of each per-unit kind — RECORDS, CHILDREN, UNIT_TOKENS,
   POSITIONS are the paying kinds (records already 2.05 MB; children 1.68,
   tokens 0.88, positions ~1.2 stored). Store each dictionary as its own
   container segment (a new consumer KIND value is format vocabulary, not a
   new record family — but confirm with the owner if in doubt).
2. Compress those segments with `ZSTD_compress_usingDict` (or CCtx + dict);
   readers decompress with `ZSTD_decompress_usingDDict` — extend
   `madc_pch::compress/decompress` or add a dict-aware sibling (ONE
   implementation; no parallel codec paths).
3. Wire the reader: `read_segment` needs the dict for dict-compressed
   segments — the segment `flags` field (reserved, 0) can carry "uses dict
   seg-id N", or pair by KIND. Keep it per-segment-general, not kind-special.
4. Measure: blob size (target ≤4.3 MB for <10 MB total binary; ELF is a fixed
   5.7 MB), bound testsubscript (watch the +50ms doesn't grow), full gate
   matrix WITH blob-presence check.
5. If dictionaries alone miss <10 MB: the remaining lever is compressing the
   3.74 MB INTERN spine (~3–5ms/compile zero-copy loss) — **OWNER DECISION,
   ask first**.

## GATE (per commit)
```
make -C src fulltest && make -C src release && MADC_BIN=bin/madc-release bash scripts/run_tests.sh
+ ls -la bin/madc-release (blob present!) + census + time bin/madc-release tests/testsubscript.mad
```
(as separate commands — no && chains in actual runs; shown compressed here.)

## SETTLED
- Slice 1 is landed and verified — build on it. The level-by-placement split
  and the raw INTERN spine are deliberate; don't "simplify" them away.
- Merge decision: the branch can merge to develop on its own green gates —
  either before or with the dictionary slice, owner's preference.
- Forest invariants: LOADED == PARSED; no new record families without owner
  sign-off; no name/kind special-casing beyond declared format vocabulary.
- Remaining forest backlog (separate from #37): ~308 drop ladder families
  (plan doc census), tasks #35 (vptr member subobjects) / #36 (dynamic vbase).
