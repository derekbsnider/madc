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
- Whether a `<` opens a template-argument list is a NAME question
  ([temp.names]/3), answered by lookup: `Program::class_member_lt_reading` /
  `unqualified_name_lt_reading` are the ONE owner; `DelimDepth::
  lt_reads_as_less_than()` reads them for every scan, the expression parser's
  qualified-name site reads them directly. `angle_open_context()` is only the
  token-level approximation for a scan with no Program. Never re-implement
  either test; never judge a `<` by the token before it.
- A scan that walks the live token stream (`nextToken` / `peekToken`)
  constructs its tracker WITH the Program — `DelimDepth d(this)` in a member,
  `DelimDepth d(&pgm)` in a helper — so the lookup reading reaches it. A bare
  `DelimDepth d;` driven by `d.update()` in such a scan is a copy of the wrong
  rule; the gate fails on it.
- A `>` inside parens opened WITHIN the argument list is greater-than, not a
  close. `DelimDepth` tracks that; a bare `--angle_depth` does not.
- Operator-function-ids (`operator<`, `operator>>`, `operator()`) are NAMES.
  Both helpers consume them opaquely — never count their symbols as delimiters.
- Char-level scans over a spelling string apply the same rule over a different
  alphabet; keep them beside the token tracker and say so in a comment.
- `scripts/check-one-delim-tracker.sh` (wired into `fulltest`) fails the build
  on a new hand-rolled copy. Migrate the scanner — never add an exemption.

See `docs/rules/delimiter-tracking.md` for the reasoning.
