# No Parallel Implementations / Anti-Drift

- One concern, ONE implementation. Do not keep a second implementation of the
  same thing (a second backend, translator, codegen path, parser) alive.
- Migration / A-B scaffolding (an old path behind a flag like `MADC_CIR_OLD`) is
  allowed ONLY if it either (a) runs in CI on every commit, or (b) carries a
  written deletion deadline. Never let it linger as untested dead code.
- Tests MUST exercise the production entry points — the same functions
  `bin/madc` calls — not a parallel or legacy path. A test that bypasses the
  live code gives false green AND cannot catch real bugs.
- Every automated test invocation runs under BOTH a wall-clock `timeout` and a
  `ulimit -t` CPU cap, so a hang (infinite loop in generated/interpreted code)
  is killed and fails the suite instead of pegging the host. The `make test`
  target and `scripts/run_tests.sh` already do this — keep it that way; never
  add an un-capped test invocation.
- When you delete a now-dead path, let the compiler confirm: build with `-Wall`
  and treat `-Wunused-function` on the deleted web as the signal that the cut
  was complete.

See `docs/rules/no-parallel-implementations.md` for the incident that motivated
this and the deeper reasoning.
