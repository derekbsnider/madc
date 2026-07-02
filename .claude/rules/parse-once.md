# Parse-Once — No Re-Parse for New Support

- madc resolves templates/generics the g++ way: parse the pattern ONCE,
  then instantiate by re-running the generic resolver over the saved tree on
  substituted args. NEVER re-lex / re-parse source text to instantiate.
- Every new C++ feature is implemented as generic resolution on the
  parse-once spine — one of the finite KINDs (call, operator/ADL, dependent
  member-type/rebind, construction) — NOT as a re-parse-dependent path.
- Parse-once tsubst is the DEFAULT (the flip is done; burndown reached 0).
  `MADC_XTEST_DEP_PARSE=0` opts back into pure re-parse — a soak escape
  hatch only, deleted together with the re-parse machinery.
- Re-parse is residual dead code awaiting deletion (deadline: Phase-5 ctor
  mem-init tsubst lands, freeing the shell). Do not build new behavior that
  depends on it.
- A feature that "works" only because the re-parse fallback catches it is
  NOT done. If it appears in the `--show-stats [why:]` fallback tally, it is
  still on the crutch — finish the KIND.
- Every change keeps the suite-wide tsubst FALLBACK count at 0. A change
  that adds a `[why:]` fallback is a regression — the ratchet
  (`scripts/tsubst_flagon_gate.sh`, wired into fulltest) gates it.
- The burndown (`scripts/tsubst_burndown.sh`) stays the regression metric:
  suite-wide HIT/FALLBACK totals + distinct `[why:]` reason-classes.
- Keep fixes generic by KIND; a fix keyed on a callee / template / operator
  NAME is wrong (see `enum-over-strings.md`, `design-principles.md` Rule #7).

See `docs/rules/parse-once.md` for the reasoning, the g++ model, the
deprecation roadmap, and the tracking metric.
