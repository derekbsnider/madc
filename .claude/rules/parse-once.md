# Parse-Once — No Re-Parse for New Support

- madc resolves templates/generics the g++ way: parse the pattern ONCE,
  then instantiate by re-running the generic resolver over the saved tree on
  substituted args. NEVER re-lex / re-parse source text to instantiate.
- Every new C++ feature is implemented as generic resolution on the
  parse-once spine — one of the finite KINDs (call, operator/ADL, dependent
  member-type/rebind, construction) — NOT as a re-parse-dependent path.
- Parse-once tsubst is UNCONDITIONAL (the flip is done; burndown reached 0;
  the `MADC_XTEST_DEP_PARSE=0` escape hatch and the slice-2/3 soak levers
  are DELETED — Phase-5 slice 4a).
- The re-parse fallback machinery is DELETED (Phase-5 slice 4b complete):
  FIRST instantiations skip their body parse like repeats, and a tsubst
  bail on a skipped body is a LOUD pre-c2mir error — never a re-parse.
  Do not reintroduce a re-parse recovery path.
- The only body parses left at instantiation are the ones that are the
  ONLY parse: pattern-INELIGIBLE sources (tsubst_eligible rejects) and
  auto-return deduction. Shrinking those = widening pattern eligibility
  per-KIND, not adding fallbacks.
- A feature that "works" only because it stays pattern-ineligible is NOT
  done. If it appears in the `--show-stats [why:]` fallback tally, it is
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
