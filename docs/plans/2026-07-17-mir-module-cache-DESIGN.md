# MIR module cache — pack the compiled form of the drained bodies

**Status: DESIGN / EXPLORATION (2026-07-17, Claude; owner said "worth
exploring"). Nothing lands without owner sign-off on the size dimension.**

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
