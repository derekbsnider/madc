# MIR module cache — pack the compiled form of the drained bodies

**Status: PROBE A MEASURED — GO numbers in hand (2026-07-17, Claude).
Awaiting owner GO/NO-GO on the size dimension before Phase 1.**

## Probe A results (2026-07-17, measured)

Corpus: the release-pack synthetic TU (19 headers from
`scripts/forest_pack_headers.txt` + empty main) frozen at HEAD — **240 units**,
the exact module every packed-binary program binds. Blob written by the
env-gated `MADC_MIR_CACHE_PROBE=<path>` hook in `CirJitSession::build_frozen`
(src/madc_cir.cpp, after `cir_compile`, before trap-prebind/link); read/run
probes are standalone C against the fork's libmir.a (tmp/probe_mir_read.c,
tmp/probe_mir_run.c, tmp/probe_zstd.c).

- **(a) Blob size: 331,337 bytes raw; 291,116 bytes zstd-19 (−12%; MIR binary
  is already dense).** Module = 9,552 items / 4,290 funcs. Against the
  10,381,208-byte packed release binary that is **+2.8%**. `MIR_write` took
  0.100s (pack-time cost, don't-care).
- **(b) MIR_read: 0.085s** (0.0848–0.0864 over 5 runs, -O2 probe, shared box
  at load ~1.9). Replaces cir_build 0.278s + c2mir 0.017s ≈ 0.295s →
  **projected bound 0.60 → ~0.39s** (the design target was 0.32–0.40).
- **(c) Stretch — the blob is EXECUTABLE, not just parseable:** in a bare C
  host with a dlsym+trap import resolver (same shape as
  `cir_prebind_frozen_traps`), MIR_read 0.079s + `MIR_load_module`+`MIR_link`
  (lazy-gen interface) 0.154s + `MIR_gen(main)` 0.0001s + `main()` → **rc=0**.
  Unresolved-to-trap imports were the expected check-gate-dropped /
  madc-hybrid-instantiation set (e.g. `num_get<int, istreambuf_iterator<int,
  char_traits<wchar_t>>>` vtables) — never called, exactly today's trap
  semantics.

**Finding for Phase 1 (new risk #7): check-clean ≠ gen-clean.** The pack
check gate runs `do_context` only; MIR codegen can still fatal. Concretely:
`--run-frozen` on a fresh **testsubscript** freeze dies with `MIR fatal error:
undeclared func reg fp` — c2mir creates the `fp` frame reg only when the
function's top scope has stack vars (c2mir.c ~18409) but four gen paths
reference it unconditionally (15327, 17420, 17794, 18138); some
testsubscript-specific def hits that combination. The RELEASE-pack module
gen+runs clean (`forest_pack.sh` already validates `--run-frozen`), so Probe A
is unaffected, but Phase 1 must MIR_write only from a gen-validated compile
(or fix the c2mir fp bug first — it is a fork bug worth fixing regardless).

## Motivation (measured 2026-07-17, testsubscript vs the standard pack)

Bound in-process 0.599s decomposes as: cir_build **0.278s (46%)** —
re-materializing c2mir node trees from the packed records for all 240 units —
plus c2mir compile 0.017s, decode+lex 0.147s, instantiate 0.142s. The records
never change between runs, so the node-materialize + compile of the packed
units produces **the identical MIR module every run**. Baselines: g++ -O0 cold
0.78s; madc optimized live 0.86s; madc bound 0.61s. This cache targets bound
≈ **0.32–0.40s** — decisively past gcc-with-PCH territory.

## The idea in one line

The forest freezes what the PARSER produced; additionally freeze what the
BACKEND produced from it — `MIR_write` the compiled module at pack time,
`MIR_read` it at bind — so packed bodies skip node-materialize + c2mir
entirely. The MIR library natively supports this (mir.h:634 `MIR_write` /
`MIR_write_module` / `MIR_read`; the upstream `c2m -o x.bmir` → `mir-run`
workflow is exactly this shape).

## Architecture: an ADDITIVE cache lane, not a new source of truth

- The forest stays the single source of truth and keeps ALL current content
  (decl surface, patterns, token recipes, body records). `--emit=c11`,
  `--dump-source`, new-specialization instantiation, and the
  loaded-must-equal-parsed doctrine are untouched.
- The MIR blob is DERIVED state, regenerated on every pack from the same
  compile the pack ALREADY runs (the check gate compiles the whole drained
  module today; --run-frozen proves the module is executable). Serializing it
  adds no new semantic surface — it is the check-gate compile, saved.
- Consumers that bind a container with no MIR blob (old corpus, or blob
  version-rejected) fall back to today's path bit-for-bit. Loud log line, not
  silent (no-silent-caps rule).

## Pack side

1. After the drain cascade converges and the check gate is clean, the pack
   already holds the final translated module. Run `gen`-less compile
   (do_context already done) → `MIR_write_module` to a buffer.
2. Store as ONE new container segment (zstd like the others). ⚠️ OWNER GATE:
   this is a pack-dimension/size decision — measure the segment size first
   (probe A below) and bring the number to the owner. Budget context: binary
   is 10.38MB today against the 10.33MB reference.
3. Version stamp = (CIR_FOREST_FORMAT_VERSION, MIR binary format version,
   MIR_COMMIT). Any mismatch → ignore blob, fall back, log.

## Bind side

1. `MIR_read` the module (fast, sequential; MIR binary is compact).
2. Consumer TU compiles through the normal path into its OWN module. Its
   references to packed bodies become MIR imports; the loaded module exports
   them. This import/export shape ALREADY exists — it is how --run-frozen
   binds "drained-library imports" (currently to trap stubs when missing).
3. Link both modules in one context (MIR_load_module × 2 + the existing
   import resolver), MIR_gen lazily as today.
4. The m&l fixpoint consults the blob's export set BEFORE deriving a body:
   packed-and-compiled ⇒ import it, skip derivation AND skip its record
   materialization. (Policy mirrors today's packed-record preference; the
   record stays in the container for emit/dump/fidelity paths.)

## What it does NOT change

- Live parse, patterns, capture — orthogonal.
- New specializations (vector<MyStruct>) — derive + compile consumer-side as
  today; they land in the consumer module and link against the blob.
- Fidelity oracles — --emit=c11 and byte-identity gates read the records, not
  the blob. The subbind/live==bound oracle still passes by construction ONLY
  if the blob's codegen equals the rebuild's codegen — which the equivalence
  gate must now also assert (new gate: bind-with-blob output == bind-without-
  blob output == live).

## Risks / open questions

1. **Blob size** (owner-gated): MIR binary of 240 units — unknown until
   measured; zstd applies. If it blows the budget, options: pack only the
   hot subset (string/stream machinery), or make the blob an optional
   sidecar file rather than appended.
2. **Duplicate definitions**: consumer must not both import AND derive the
   same symbol (MIR redefinition). The export-set check (bind step 4) is the
   guard; needs a loud error if a derivation slips through.
3. **Cross-module inlining**: MIR-gen inlines at the MIR level post-link, so
   packed bodies should still inline into consumer code — verify in probe B
   (perf parity of a hot loop calling a packed body, blob vs rebuild).
4. **Position/diagnostic fidelity**: MIR carries coarser positions than the
   records. Runtime diagnostics for packed bodies degrade slightly — bind
   errors attributed to defs (MADC_CHECK_ATTRIB) only exist for the rebuild
   path. Acceptable for a cache with a fallback; document it.
5. **Fork coupling**: MIR binary format rides MIR_COMMIT. Pin discipline
   already covers it (build.md).
6. **c2mir tree mutation**: do_context mutates trees; the pack must
   MIR_write from the SAME compile the check gate validated, not a second
   translate (or re-translate and re-check — same rule as the check gate's
   round discipline).

## Phase 1 status (2026-07-17)

- **Rungs 1+2 LANDED @1f2694b8**: `--freeze-mir-cache` packs the module as
  optional segment `CIR_FOREST_SEG_MIR_MODULE` (slot 20; stamp = forest
  version + MIR API level; gen-fatal containment verified on testsubscript's
  fp bug); `--run-frozen` consumes it — **4.14s → 0.97s (−77%)** on the
  240-unit pack; `MADC_NO_MIR_CACHE=1` is the A/B lever;
  `forest_selfexe_gate` asserts cache == no-cache output. Release binary
  10,686,704 (+2.9%, the approved trade); packed suite 697/0/0/16.

## Rung 3a status (2026-07-17) — bind-lane import short-circuit LANDED

Implemented with ONE deviation from the design below, chosen after an item
census of the real pack module: the blob stays a WHOLE module (rung-2 format
unchanged, `--run-frozen` untouched, old corpora stay valid) and the
funcs-only strip happens IN-MEMORY at bind, after `MIR_read`, via a new fork
API. Census facts that made this cheap (tmp/probe_item_census.c): 4290 funcs
(all exported, incl. main/`__madc_global_init`), only 11 exported data items
(std tag globals — `in_place`, `piecewise_construct`, `__default_lock_policy`,
…), zero ref_data/expr_data, zero anonymous tails after exported data (tails
only follow private string-literal sections). No vtables — the pack TU
instantiates no polymorphic classes; consumer-side vtable emission is
untouched either way.

- **Fork API** `MIR_module_privatize_for_link(ctx, m, unexport_names, n)`
  (mir.h/mir.c): un-exports the named funcs (defs stay as unexported dead
  weight — no unlinking) and converts every exported named data item to an
  import IN PLACE — the interned name pointer is reused as the import id, so
  the module item table (which hashes the name pointer) and every insn REF
  operand stay valid. Export list-items of privatized names are unlinked.
  Idempotent; returns the count of unsplittable sections (exported data with
  an anonymous continuation item — refuses those; caller falls back). Data
  redefinition across modules is SILENT split-state in MIR (mir.c
  `MIR_load_module` only fatals func redefs), which is exactly why
  consumer-sole-owner is enforced by conversion, not by load order.
- **Bind lane** (`CirJitSession::build`): stages the cache PRE-translate
  (read blob → privatize entry points → collect func exports into
  `prog->mir_cache_exports`) so every fallible step fails while the fallback
  is still clean; the m&l fixpoint (cir_builder.cpp forest_lazy stage) emits
  the forward proto but SKIPS the def for cache-exported symbols (proto-less
  shapes keep their def — self-healing); post-translate, any consumer-defined
  overlap (eager user funcs in full-program corpora) is un-exported from the
  cache module — consumer wins every overlap by construction, no
  `MIR_set_func_redef_permission` needed; `load_and_link` loads the cache
  module first, then the consumer, then trap-binds cache-ONLY unresolvable
  imports (`cir_prebind_cache_traps` — a name the consumer also imports stays
  strict, preserving the live-compile failure surface).
- **3a keeps materialization + the callee-walk** (consumer emission surface =
  live minus the skipped defs) — the wall win is rung 3b (skip `node_for` for
  cache-exported syms; transitively-referenced bodies then never materialize).
- Gates: packed suite 697/0/0/16 (release binary, cache active on every
  test); bind gate 18/18; 3-way equivalence cache == no-cache == live on
  testsubscript/testfreezerun/testforeach2; emit lane byte-identical with the
  lane on/off (cache is JIT-bind-only by construction); `forest_pack.sh` now
  asserts engagement (staged-line grep) + cache==no-cache equality on every
  release pack. Release binary 10,690,832 (+4,128 vs rung 2 — code only).
  testsubscript imports 38 bodies instead of emitting them (4288 importable).

## Rung 3 design — forest-bind m&l short-circuit (recon; 3a landed above, 3b open)

The bind lane's cost is `forest_lazy` (cir_builder.cpp ~19394): restored
`has_forest_body` symbols materialize node records into the consumer tree
inside the m&l fixpoint — the 0.278s cir_build bucket. Short-circuit: skip
materializing any symbol the blob exports; emit only the forward proto
(`forest_fwd_proto` needs the def's op0/op1 — a proto-only materialization,
or serialize proto shapes alongside the blob); the consumer's reference
becomes a MIR import; load the blob module alongside and MIR_link resolves.

**The duplicate-item problem (measured facts, mir.c):** `MIR_load_module`
FATALS on a cross-module exported-func redefinition unless
`MIR_set_func_redef_permission` (mir.h:567) is on; named DATA items loaded
twice get TWO addresses — split state (a mutable global duplicated between
blob and consumer is a correctness bug, not a link error). The blob today
also carries the pack TU's `main` and `__madc_global_init`, which every
consumer defines.

**Chosen shape — funcs-only blob (single owner for every named datum):** at
pack, before MIR_write_module, strip the module to code: remove `main` +
`__madc_global_init` outright; replace every NAMED data/bss/ref_data/
expr_data item with an import of that name (anonymous items — string
literals — stay, they cannot collide). The consumer remains the sole
definer of all named data (vtables, tag globals); blob bodies import them,
exactly like library code relocating against program-owned symbols. No
redefinition permission needed for data; func overlap disappears because
the consumer stops emitting exactly the blob-exported set. Residual risk:
a blob body referencing a named datum the consumer's referenced-surface
emission never emits → trap import (dlsym fallback catches real libstdc++
symbols first); the equivalence gate (bind-with-blob == bind-without ==
live, plus the bind gate 18/18) is the arbiter.

## Phased plan

- **Probe A (cheap, 1 sitting):** at --freeze, after the check-gate compile,
  MIR_write the module to tmp/; report (a) blob size raw+zstd, (b) MIR_read
  wall time standalone, (c) MIR_read+gen+run of testsubscript's main against
  the loaded module. No container integration. GO/NO-GO numbers for the
  owner: expected read ≪ 0.1s vs the 0.28s rebuild it replaces, size ≈ MIR
  binary is typically smaller than the source records.
- **Phase 1:** container segment + version stamp + bind-side load + export-set
  short-circuit in the m&l fixpoint + fallback path + new equivalence gate
  (blob vs no-blob vs live outputs identical).
- **Phase 2:** packed suite + full matrix + bench rows; owner sign-off on
  size; only then default-on.

## Relationship to the roadmap

This is the first concrete step of the banked AOT track (ADR 0001 names
--emit=c11 / JIT / interp as three outputs of one IR; a serialized MIR module
is the fourth: the object-file output). It also compounds with per-project
freezes: a SMAUG project pack with a MIR blob gives all 51 TUs near-instant
library startup.
