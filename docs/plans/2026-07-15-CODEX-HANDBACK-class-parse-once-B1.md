# CODEX HANDBACK - class-KIND parse-once B1

**Date:** 2026-07-15
**Branch:** `feature/class-parse-once-codex`
**Base:** `develop` @`3f529a7a`
**B0:** `c6f05b48`
**B1:** this checkpoint commit

## What changed

- Added the immutable `ClassPatternArena` and one-time primary/partial template
  definition capture, normalization, stable identities, and semantic/token
  fingerprints.
- Extended the existing class/partial `TEMPLATE_PAYLOAD` in CIR forest format
  v28 and restored patterns directly with fingerprint validation. No new
  segment or top-level record kind was introduced, and bind-time reparse is
  absent.
- Kept every captured pattern deliberately ineligible. B1 therefore changes no
  instantiation dispatch: the exact B0 class census remains pattern 0, parse
  48604, cache 99334, opaque 24430, with only `pattern-not-captured`.
- Added scoped keyed-map transactions for the registration journal and focused
  unit coverage. Corrected the class-ratchet and warning-census runners to
  honor generic fixture input/argv/timeout and expected-error conventions.

## Validation

- `make -C src fulltest`: **695 passed, 0 failed, 0 timed out, 16 skipped**.
- `make -C src release`: rc 0, 240 packed units; `bin/madc-release` is
  **10,176,152 bytes** with `MADCSNAP` footer magic.
- Packed release suite: **695/0/0/16**.
- Forest bind gate: **18/18**, including `[subbind]`; self-exe, index, and DM
  oracles green.
- Tsubst matrix: **13/13**; ratchet **10 hit / 0 fallback**.
- Focused units: `test_cir_freeze` **36/36, 740 assertions**;
  `test_stringpool` **7/7, 10,032 assertions**.
- Host compiler warnings: **0** in the complete debug/PIC build and the
  `-Wall -O2` release build. Source warning census: **711 compiled, 0 warnings**.

The full 690-test class census produced the exact checked-in baseline. Its only
nonzero exit came from the runner treating three existing `.expect_err`
fixtures as ordinary successes. The generic fixture handling was fixed and
validated against an expected-error fixture; the already-proven 690-test scan
was not repeated.

## Remaining

Stop here for independent B1 verification. After approval, B2 enables only the
narrow basic structural pattern lane, with transactional rollback and a loud
failure for an admitted pattern; parser retry remains forbidden. B3-B6 remain
open under the approved implementation handoff.

The `class_kind_parse_once` KG decision and the `forest_perf_ladder` milestone
were updated first. Repo mirrors updated here are `claude_status.json`,
`docs/plans/ROADMAP.md`, and `docs/test-status.md`; `CHANGELOG.md` is reserved
for the B3/B6 checkpoints per the handoff.
