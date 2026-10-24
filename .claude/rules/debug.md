# Debug Output Rules

- All debug-only statements wrapped in `DBG(x)`:
  `DBG(cout << "trace: " << value << endl);`
- `DBG(x)` has ONE definition, in `include/datadef.h` (the do-while(0)
  idiom is required — no exceptions); a source file never defines its own.
  Gated by `scripts/check-one-dbg-macro.sh`.
- `madc_verbose` (thread_local) is declared in `include/datadef.h` and
  defined in `src/madc_globals.cpp`. Its ONE writer is `madc_set_verbose(v)`
  (`-v` / `--verbose`, the embedding verbose scope), which also sets the
  process-wide pre-test `madc_verbose_requested` that DBG reads first.
- Never remove existing `DBG()` calls — they are essential diagnostics.
- Never gate error output behind `DBG`. The `throwit` / `throwstream`
  mechanism always prints to stderr.
- Multi-statement DBG blocks that share a local must be a SINGLE
  `DBG(...)` call (the do-while(0) creates a scope per invocation).
- Unit test files must define both `thread_local bool madc_verbose = false;`
  and the `DBG(x)` macro before including project headers (their own binary;
  the gate exempts tests/unit).

See `docs/rules/debug.md` for the reasoning and worked examples.
