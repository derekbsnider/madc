# SELF-HANDOFF — Probe A (MIR module cache) then task #41 (live capture)

**For:** post-compaction Claude, cold start. **Written:** 2026-07-17 by Claude.
**Branch:** `feature/class-parse-once-codex` @19b498ba (pushed; tree clean
except foreign untracked `mir-debug-support.md` — NEVER stage it).
**Read fully before acting. Then execute in order. Owner is the arbiter of
scope changes; if told to wrap up: commit, push, hand back immediately.**

## Session state (all verified, all pushed)

- **R1–R4 of the class-pattern perf ladder are COMPLETE.** R4 (@5a5f4773)
  deleted BOTH fences (pack force-legacy + eligibility member-template-
  overloads); root cause was token-SPELLING parity — see
  `docs/plans/2026-07-16-R4-overload-replay-FINDINGS.md` (full evidence chain
  + the three fixes). Validation: fulltest 697/0/0/16; bind gate 18/18 incl.
  subbind on an UNFENCED pack; packed suite 697/0/0/16 + MADCSNAP blob;
  binary 10,381,208 bytes (reference for growth checks).
- **Bench state (testsubscript):** dev -O0 live 2.257 / bound 0.613 opt-in at
  load 1.00 (TSV row @0f979fb8). KEY REFRAME (recon 2026-07-17): optimized
  binary live (`bin/madc-release --no-forest-bind`) = **0.86s** vs g++ -O0
  cold **0.78s**; bound **0.61s BEATS g++ by 22%**. The 2.x numbers are the
  -O0 dev-binary trend metric only. Bound decomposes: cir_build 0.278s (46%),
  decode+lex 0.147, instantiate 0.142, c2mir 0.017.
- **B3 wall gate (dev binary: live<2.17, bound<0.593) still open** — 90ms/20ms
  short. Remaining levers: #41 (below), R5 eligibility widening
  (`__iterator_traits`/`allocator_traits` ×14, `__move_if_noexcept_cond`
  parse-error ×7), and the MIR cache (bound).
- Baseline worktree `/workspace/madc-ab` (ea4f4021 binaries, for interleaved
  A/B). Saved binaries `tmp/madc-b2b2cf5e`, `tmp/madc-release-b2b2cf5e`.
  Corpora: `tmp/r4_fix.msnap` (current-format testsubscript pack, binds
  green). Live output reference: `tmp/r4_sub_live.out`.

## MISSION 1 — Probe A: MIR module cache GO/NO-GO numbers

Design: `docs/plans/2026-07-17-mir-module-cache-DESIGN.md` (owner said "worth
exploring"; the SIZE dimension is owner-gated — probe produces the numbers,
owner decides). Deliverable: three numbers, no container integration.

1. Hook: env-gated (`MADC_MIR_CACHE_PROBE=<path>`) `MIR_write` of the
   compiled module. Cleanest place: the `--run-frozen` path — it thaws and
   compiles the WHOLE packed module through `cir_compile` (madc_cir.cpp ~68,
   `c2mir_compile_tree`). After a successful compile, the MIR module lives in
   the MIR context: `MIR_write(ctx, fopen(path))` (API mir.h:634; or
   `MIR_write_module` for the single module). Gate strictly by env; zero
   default cost (gated-debug convention).
2. Run `MADC_MIR_CACHE_PROBE=tmp/probeA.bmir bin/madc --run-frozen=tmp/r4_fix.msnap`
   (expect the known cosmetic tail "MIR fatal error: undeclared func reg fp" —
   it happens at EXECUTION, after compile; write the blob BEFORE gen/exec).
3. Report: (a) `ls -l tmp/probeA.bmir` + `zstd -19 -k` size; (b) MIR_read
   wall: tiny env-gated read path (`MADC_MIR_CACHE_READ=<path>` → MIR_init +
   MIR_read + report seconds, exit) — or a 30-line probe main linking
   /workspace/mir/libmir.a; (c) optional stretch: gen+call main from the
   loaded module.
4. GO shape: read ≪ 0.1s (vs 0.28s rebuild it would replace), zstd size small
   enough for the ~10.4MB budget conversation. Write the three numbers into
   the design doc, commit, push, REPORT TO OWNER, stop there (Phase 1 needs
   the owner's size sign-off).

## MISSION 2 — task #41: live lazy capture of std::vector fails

- Symptom (probe `MADC_CLASS_PATTERN_PROBE=vector`, live with
  `MADC_CLASS_PATTERN_LIVE=1`): `capture FAILED: std::vector (poisoned) at
  stl_vector.h:428 — detail: no member named '_M_impl'`. Consequence: the
  flagship containers NEVER pattern-serve live → live lane can't beat the
  2.17 gate. At PACK the same capture SUCCEEDS (eager at definition time);
  the failure is specific to the LAZY live context (2nd-demand trigger,
  mid-consumer-parse).
- Repro: `MADC_CLASS_PATTERN_LIVE=1 bin/madc tmp/red_r4_dataptr.mad`
  (reducer exists; iostream must precede vector — include-order bug is
  pre-existing/unrelated).
- Where: capture = `capture_class_pattern` (parser.cpp) — production-parser
  pass over the renamed body `__madc_class_pattern_<hash>` with OPEN
  typeparams. `this->_M_impl` must resolve against the dependent base
  `_Vector_base<_Tp,_Alloc>` (a dependent SHELL at capture time). Hypothesis
  to verify FIRST (trace, don't probe-loop): at pack/definition time the
  dependent shell for `_Vector_base<_Tp,_Alloc>` exists with its members
  (just parsed); at lazy-capture time (mid consumer parse) the shell lookup
  misses or the shell lacks members. Diff the two contexts with the probes
  (capture probes print to cout — capture MUTES cerr).
- NOTE: R4's fixes (registered-identity substitution tokens, base-class
  deduction) touched what capture bodies parse — re-verify the failure still
  reproduces at HEAD before diagnosing (verify-over-stale-handoff).
- Acceptance: vector/map/string capture OK live; live == parse-lane output
  identical (equivalence oracle); fulltest + packed suite green; bench row
  (expect live opt-in to drop toward/below 2.17 — that plus R5 widening is
  the default-flip case).

## Standing constraints (unchanged, binding)

- Commits: `git commit -F -` heredoc, BARE `EOF` terminator, trailers
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` +
  `Claude-Session: https://claude.ai/code/session_01G1BafJ7iorF9bRB9K3WVfZ`.
  NEVER `git add -A`. No `&&` chains. Builds/suites run_in_background
  (foreground 10-min cap kills them). Scratch in `tmp/`.
- Packed suite = THE arbiter; verify MADCSNAP magic in the binary tail.
  Never re-run a suite on content already proven green.
- Bench discipline: this NAS shows host load madc can't see — single-binary
  TSV rows only at load < 2 (`uptime` first; wrapper loop pattern in
  tmp/ scripts); cross-revision comparisons use the INTERLEAVED A/B script
  pattern (tmp/ab_bench_r3.sh shape), medians of 7.
- Debug levers that cracked R4 (reuse, don't rediscover):
  `MADC_CHECK_ATTRIB=1` (names failing defs at bind),
  `-DMADC_DEBUG_FNTPL=1` build + `MADC_DEBUG_FNTPL_DUMP=<substr>`,
  `MADC_CLASS_PATTERN_PROBE`, `MADC_MTI_PROBE`, `MADC_MTI_PROBE_CLASS`,
  `MADC_CLASS_PATTERN_NO_MEMO`. Final validation builds = default CXXFLAGS.
- Owner gates: pack/binary size dimensions; new record families; default
  flips (R5 needs sign-off + the 10,381,208 size number re-checked).
- Mirror sync at milestones: memory topic file
  (feedback_forest_load_never_reparse.md) + MEMORY.md line + KG
  `forest_perf_ladder` (scripts/kg_query.sh, property `summary`) +
  claude_status.json.

## Backlog after missions 1+2

R5 (eligibility widening + default flip + B3 gate close, owner checkpoint) →
MIR cache Phase 1 (if owner GO) → SMAUG --project bench (the real-program
proof the owner asked about) → tasks #35, #36.
