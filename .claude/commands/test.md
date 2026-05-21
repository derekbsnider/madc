# /test — Build and run the full test suite

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
