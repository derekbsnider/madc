# /test — TIER 3: build and run the full test suite

This is the MERGE-WAVE battery — hours, run once at the arc's release boundary.
It is one of THREE tiers (`.claude/rules/testing-fulltest.md`); running it for
an ordinary change is the wrong tier, and so is skipping the middle one:

  TIER 1  targeted, per change     — `bash scripts/run_tests.sh <names>`
  TIER 2  per COMMIT, <3 minutes   — `bash scripts/fast_lanes.sh`, or `/commit`
  TIER 3  per MERGE WAVE           — this command

Run the full build + test pipeline and report results.

## Steps

1. **Build and run all tests**:
   - Run `make -C src fulltest`
   - Capture both stdout and stderr

2. **Report results**:
   - Number of integration tests passed/failed
   - Number of unit tests passed/failed
   - Any build warnings or errors
   - List any failing tests by name

3. **(Optional — only if user passes `--exe`)** Also run the native EXE test lane:
   - Run `bash scripts/run_tests.sh --exe`
   - Report EXE-mode pass/fail counts alongside JIT results
