# Testing: fulltest gates the MERGE WAVE, targeted tests gate the change

Incremental changes get TARGETED validation: the new/affected tests plus the
touched subsystem's neighbors — never the full battery per commit.
Run `make -C src fulltest` ONCE per merge wave, at the release/merge gate
(or when a change's blast radius is genuinely suite-wide, e.g. lexer/include
machinery, shared codegen).
NEVER re-run a suite on content that is already green — ceremonies are git-only.
When work touches native executable, AOT, runtime-parity, or shared codegen paths,
the merge-wave battery also includes `bash scripts/run_tests.sh --exe`.
Do not leave the tree with JIT green and EXE broken, or EXE green and JIT broken.
Do NOT run integration tests in a shell loop — use the Makefile target.
