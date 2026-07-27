# Delimiter Tracking — one shared scanner for `(` `[` `{` `<`

- NEVER hand-roll balanced-delimiter bookkeeping in a token scan. No new
  `++paren_depth` / `--angle_depth` / `>>`-splitting if-else chains.
- Use `DelimDepth` (`src/parser.cpp`, forward-declared in `include/madc.h`):
  - index form — `delim_scan_step(seq, i, d)`, returns tokens consumed
  - stream form — `Program::delimStepStream(t, d, extra)`
  - `d.top()` for "outside every delimiter"; `d.paren` / `d.square` /
    `d.brace` / `d.angle` to test one axis
- Layer caller-specific logic (comma counts, terminators, spelling capture)
  ON TOP of `DelimDepth` — never inside it.
- A `<` opens a template-argument list ONLY in a template-id head context.
  `DelimDepth::angle_open_context()` owns that test — do not re-implement it.
- A `>` inside parens opened WITHIN the argument list is greater-than, not a
  close. `DelimDepth` tracks that; a bare `--angle_depth` does not.
- Operator-function-ids (`operator<`, `operator>>`, `operator()`) are NAMES.
  Both helpers consume them opaquely — never count their symbols as delimiters.
- Char-level scans over a spelling string apply the same rule over a different
  alphabet; keep them beside the token tracker and say so in a comment.
- `scripts/check-one-delim-tracker.sh` (wired into `fulltest`) fails the build
  on a new hand-rolled copy. Migrate the scanner — never add an exemption.

See `docs/rules/delimiter-tracking.md` for the reasoning.
