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
