# Testing: Use make fulltest

After making changes, always run `make -C src fulltest` to verify.
This runs both unit tests (doctest) and all integration tests in one command.
When work touches native executable, AOT, runtime-parity, or shared codegen paths,
also run `bash scripts/run_tests.sh --exe`.
Do not leave the tree with JIT green and EXE broken, or EXE green and JIT broken.
Do NOT run integration tests in a shell loop — use the Makefile target.
