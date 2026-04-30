# Documentation vs Rules — Separation of Concerns

- `.claude/rules/*.md` files contain ONLY bare rules — prescriptive,
  imperative, scannable. No multi-paragraph explanations, no "why", no
  history, no code examples beyond a minimal one-liner if absolutely
  required to disambiguate.
- `docs/rules/*.md` files contain the reasoning behind each rule: the
  failure modes that motivated it, worked examples, historical context,
  long code samples.
- Every non-trivial rule file in `.claude/rules/` has a sibling file in
  `docs/rules/` with the same base name.
- Bare rules point to their docs file with: "See `docs/rules/<name>.md`
  for the reasoning."
- Never duplicate content between the two. If a fact appears in both,
  one of them is wrong — pick the right home.
- When adding a new rule: start with the rules file (one-line bullets).
  If you feel the need to explain, move that to `docs/rules/`.
- When maintaining: if a rules file exceeds ~30 lines or contains a
  sentence starting with "because", "why", "historically", or
  "originally" — it needs splitting.
