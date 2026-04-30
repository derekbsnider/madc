# Debug Output Rules

- All debug-only statements wrapped in `DBG(x)`:
  `DBG(cout << "trace: " << value << endl);`
- `DBG(x)` is defined in every source file as
  `#define DBG(x) do { if(madc_verbose){x;} } while(0)` (the do-while(0)
  idiom is required — no exceptions).
- `madc_verbose` is declared in `include/datadef.h` and defined in
  `src/madc.cpp`; `-v` / `--verbose` sets it.
- Never remove existing `DBG()` calls — they are essential diagnostics.
- Never gate error output behind `DBG`. The `throwit` / `throwstream`
  mechanism always prints to stderr.
- Multi-statement DBG blocks that share a local must be a SINGLE
  `DBG(...)` call (the do-while(0) creates a scope per invocation).
- Unit test files must define both `bool madc_verbose = false;` and the
  `DBG(x)` macro before including project headers.

See `docs/rules/debug.md` for the reasoning and worked examples.
