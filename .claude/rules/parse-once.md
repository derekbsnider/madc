# Parse-Once — No Re-Parse for New Support

- madc resolves templates/generics the g++ way: parse the pattern ONCE,
  then instantiate by re-running the generic resolver over the saved tree on
  substituted args. NEVER re-lex / re-parse source text to instantiate.
- Every new C++ feature is implemented as generic resolution on the
  parse-once spine — one of the finite KINDs (call, operator/ADL, dependent
  member-type/rebind, construction) — NOT as a re-parse-dependent path.
- Re-parse is a TRANSITIONAL fallback (bail-net) only, gated by
  `MADC_XTEST_DEP_PARSE`. It carries a deletion deadline: suite-wide
  burndown = 0. Do not build new behavior that depends on it.
- A feature that "works" only because the re-parse fallback catches it is
  NOT done. If it appears in the `--show-stats [why:]` fallback tally, it is
  still on the crutch — finish the KIND.
- Every change moves the suite-wide tsubst FALLBACK count DOWN or FLAT,
  never up. A change that adds a `[why:]` fallback is a regression — gate it.
- Track distance-to-goal with the burndown (`scripts/tsubst_burndown.sh`):
  suite-wide HIT/FALLBACK totals + distinct `[why:]` reason-classes (the KIND
  worklist). 0 fallback → flip the default → delete the re-parse path.
- Keep fixes generic by KIND; a fix keyed on a callee / template / operator
  NAME is wrong (see `enum-over-strings.md`, `design-principles.md` Rule #7).

See `docs/rules/parse-once.md` for the reasoning, the g++ model, the
deprecation roadmap, and the tracking metric.
