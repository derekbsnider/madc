# Rule Trailers — show the Top 5 work, don't assert it

- Every commit touching `src/` or `include/` MUST carry these four trailers.
  `scripts/check-rule-trailers.sh` (in `fulltest`) fails the build without them.

```
Hypothesis: <what you believed was wrong, written BEFORE editing>      (#3)
Layer:      <the layer chain, and why the one you edited is deepest>   (#2,#5)
Searched:   <the grep you ran, the CONCEPT, and what came back>        (#4)
Oracle:     <what gcc/clang did on a reducer, and what madc did>       (#1)
```

- `n/a — <reason>` is a permitted value. Silence is not. An empty or
  whitespace-only field fails.
- Write each trailer as a terse factual claim — the code fact, a noun phrase,
  an arrow chain — NEVER a first-person account of investigating ("I believed
  …", "then I grepped …", "I realized …"). The gate checks only that the field
  is present and non-empty; the narrated form adds nothing it wants and trips
  an output-side "reasoning extraction" classifier. Same for CHANGELOG entries:
  behaviour facts, no discovery narrative.
- On an Opus 5.5 session the classifier trips on these trailers even when
  terse, repeatedly — the trip is model-specific to 5.5's output. DELEGATE the
  `git commit` to a subagent on a different model (Agent tool, `model` override
  — sonnet or opus-4-8): hand it the staged diff and the four facts, it writes
  the trailers and commits. Do not reword-and-resend on 5.5.
- **`Layer:` is the one that catches shims.** Write the chain
  (`caller -> helper -> root cause`) and state which you edited. If you cannot
  say why yours is the deepest, you are shimming — stop and go lower.
- A true principle is not a substitute for the deepest layer. "Scanners need
  extent, parsers need validity" was true AND was cover for a shim.
- `Searched:` names the CONCEPT, not the identifier already in your head:
  "canonicalize a path" not `realpath`; "balanced delimiter tracker" not
  `DelimDepth`.
- `Oracle:` for a pure refactor is `n/a — behaviour-preserving, suite is the
  oracle`. For any behaviour change it must name the reducer and both outputs.
- Do not weaken the gate to pass. Move `EPOCH` FORWARD only, never backward,
  and never to skip a commit you did not want to justify.

See `docs/rules/rule-trailers.md` for the reasoning.
