# /test — TIER 3: the seam battery

This is the MERGE-WAVE battery — about an hour, run once at the arc's release
boundary. It is one of THREE tiers (`.claude/rules/testing-fulltest.md`);
running it for an ordinary change is the wrong tier, and so is skipping the
middle one:

  TIER 1  targeted, per change     — `bash scripts/run_tests.sh <names>`
  TIER 2  per COMMIT, <3 minutes   — `bash scripts/fast_lanes.sh`, or `/commit`
  TIER 3  per MERGE WAVE           — this command

Name the seam it gates in one sentence before launching it.

## Steps

1. **Run the battery** (from the NAS checkout; every stage runs on the build
   container through `scripts/remote_build.sh`), detached, with ONE
   self-exiting background waiter:
   - `setsid nohup bash scripts/seam_battery.sh > tmp/logs/seam.out 2>&1 < /dev/null &`
   - wait for `=== seam battery done ===` in `tmp/logs/seam.out`
   - It runs cheapest first: pre-build every toolchain + static gates, `gates`
     (unit tests + every gate, no suite), the FULL suite on the shipped -O2
     packed binary with no headers on disk (`headerless`, then
     `headerless-win`), the headerless-skipped subsets with headers on disk
     (`ondisk`, `ondisk-win`), `exeobj` (--exe --obj on the packed binary),
     then `release-macos` and `aarch64-ld`.

2. **Report results** by stage: pass/fail tallies, every failing test by name,
   the red stages from the summary block, and any build warnings.

3. **Record** each green lane on the NAS with `bash scripts/lane_ledger.sh
   record <lane> "<tally>"` (linux-battery, headerless-win, wine64, macos,
   aarch64-ld, tests-jit), then `scripts/lane_ledger.sh check --promote`.
