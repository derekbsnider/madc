# Scratch Files

- All scratch, temporary, and reducer files go in `tmp/` at the repo root.
- Never create `*_tmp.mad`, `*_tmp.inc`, `*_tmp.c`, or other scratch files
  directly in `tests/` or the repo root.
- A test removes every file it creates before it exits; `scripts/run_tests.sh`
  fails a test whose JIT run leaves a new name in the working directory.
- `tmp/` is gitignored — nothing in it is tracked or committed.
- Integration tests promoted from scratch work get renamed (drop the
  `_tmp` suffix) and moved to `tests/` with proper fixtures.
