# /dupaudit — Duplication recon audit

Argument: `$ARGUMENTS` — a subsystem name, a path, or a file glob. Empty means
"the code this branch touched" (derive it from the diff against `develop`).

Hunt **semantic** duplication: N sites implementing ONE rule, where at least one
of them differs. That divergence is the bug — the redundancy is only the cost.

This is not a clone detector. Textual clone tools find copy-paste and miss the
case that actually hurts here: divergent reimplementations that share no text.
The angle-bracket disaster (six `++angle_depth` scanners, one guarded, a sixth
unguarded copy added two days *after* the fix landed in the first) had no
duplicated text at all. What exposed it was a **bookkeeping marker**.

## When to run

At **feature-merge time, scoped to the subsystem the feature touched**. That is
where new copies are born — someone working in an area without full knowledge of
it. Running it there would have caught the sixth angle scanner in two days
instead of seven weeks.

Also run it whenever a fix is about to land in something that "feels like it
might exist elsewhere." That instinct is usually right.

## Steps

1. **Fix the scope and say it out loud.**
   - With an argument: use it.
   - Without: `git diff develop...HEAD --name-only` → the touched files, widened
     to their subsystem (a change in `parseAddressOfExpression` scopes to
     parser.cpp's expression paths, not to all 50k lines).
   - Report the scope before starting. A sweep whose scope nobody stated is a
     sweep nobody can trust or repeat.

2. **Re-check known families first — cheap, and it catches regrowth.**
   - Query the KG for `DupFamily` nodes whose scope overlaps:
     `scripts/kg_query.sh -ro "MATCH (f:DupFamily) RETURN f.name, f.marker, f.sites, f.status"`
   - Re-run each family's `marker` grep. Compare the site count to `f.sites`.
   - **A count that GREW is a regression — report it first, above anything new.**
     A family that has grown since it was recorded means the rule leaked again.
   - A family marked `gated` should be impossible to grow; if it grew, the gate
     is broken and that is the finding.

3. **Discovery pass over the scope.** Look for one rule with several
   implementations. Productive markers, in rough yield order:
   - **Bookkeeping counters/state**: `++angle_depth`, `paren_depth`, depth or
     nesting trackers, cursor/checkpoint variables.
   - **Repeated lookups of the same map/registry**: `namespace_map.find(`,
     `datatype_map.find(`, `findVariable(` for the same *purpose*.
   - **Repeated predicate shapes**: several functions answering the same
     question (`is_system_header_path`-style classification, "is this a template
     id", "does this qualifier name a scope").
   - **Repeated fallback/default tables**: the same literal list written more
     than once (three private copies of one include fallback list, 2026-07-27).
   - **Parallel resolution paths**: two functions resolving the same syntax in
     different contexts (`parsePostfixChain` and `parseAddressOfExpression` both
     resolving a qualifier before `::`).
   For a scope beyond ~3 files, fan out read-only recon subagents (sonnet is the
   right tier) — one per file or per concern — and synthesize. Invoking this
   skill authorizes that fan-out.

4. **Apply the tie-breaker to EVERY candidate before reporting it.** This repo
   holds two rules that pull against each other — "no parallel implementations"
   and "separation of concerns" — and the audit is worthless if it can't tell
   them apart:

   > **Would a change to the rule require editing more than one site?**

   - Yes → duplication. Report it.
   - No, and the sites would legitimately evolve apart → two concerns that
     happen to look alike. **Do not report it.** Recommending that merge is
     worse than the duplication.

   State the answer explicitly for each reported family. A candidate that cannot
   pass this test does not go in the report.

5. **Rank by damage, not by count.**
   - **First: families where one site DIFFERS.** That is a live bug. Say what
     the divergence actually does when hit.
   - Then: families that are merely redundant (all sites agree today, but the
     next edit has N places to miss).
   - Last: cosmetic repetition.

6. **Cap the report at THREE families.** Say plainly what was dropped and why.
   An audit that emits fifty findings produces zero fixes.

7. **Before recording a marker, prove it is a good one.** Every undercount this
   audit has produced came from a bad marker, and a bad marker is worse than no
   marker — it reports a smaller family with confidence.

   - **Match the CONCEPT, not one spelling of the bookkeeping.** The
     angle-bracket family was counted at 6, then 8, then 27. `++angle_depth`
     missed a counter named plain `depth`; it also missed `DelimDepth`'s own
     `++angle` — *the consolidated implementation itself* — and every
     paren/square/brace-only scanner, which are the same rule with one axis
     dropped. The concept was "a local balanced-delimiter counter."
   - **Count IMPLEMENTATIONS, not USES.** `sys_include_table_consumers` was
     seeded with a grep counting *callers* of the consolidated accessor. A
     rising caller count is good news, but the marker read it as regrowth. For
     a consolidated family, assert instead that the OLD token appears nowhere
     outside its owner.
   - **Sanity-check before recording:** does the marker match the known-good
     implementation too? If not, it is keyed on a spelling. Widen it, re-count,
     and report the number the WIDER marker gives.
   - **Search for the consolidated owner before declaring a family homeless.**
     Twice now the single shared implementation already existed and simply had
     not been adopted — in which case the finding is "N sites ignore X", not
     "N sites need a new X", and the fix is adoption, not extraction.

8. **Record every reported family in the KG**, so the next sweep re-checks
   instead of rediscovering:
   ```
   MERGE (f:DupFamily {name:'<slug>'})
   SET f.rule = '<the one rule, in a sentence>',
       f.marker = '<the grep that finds every site>',
       f.sites = <count>, f.scope = '<subsystem>',
       f.divergent = '<which site differs, or none>',
       f.status = 'open'   // -> 'consolidated' -> 'gated'
   ```

9. **Report per family:**
   - the rule, in one sentence
   - every site as `file:line`, marking which one differs
   - what the divergence does when hit (or "none — redundant only")
   - the tie-breaker answer
   - a consolidation sketch: what the single owner would be
   - **whether a gate is possible, and what it would grep for**

## Non-goals — do not do these

- **Do not fix anything.** This is recon. Findings become planned work with
  their own gates; a consolidation done mid-audit is ungated by construction.
- **Do not merge things that only look alike.** See step 4.
- **Do not report a family you have not verified by reading every site.** A
  grep hit is a candidate, not a finding.

## The discipline this serves

Discovery is expensive and finds a family once. A gate is free and means that
family can never regrow. So **every consolidation leaves a gate behind**
(`scripts/check-no-std-hardcoding.sh` and `tsubst_flagon_gate.sh` are the
precedents — both wired into `fulltest`).

That is what makes this compound: over time, more families move `open` →
`consolidated` → `gated`, and the sweeps should find less. If they don't, that
is itself the signal.

A rule without a gate decays. The angle-bracket rule was diagnosed, fixed, and
**commented** — and still lost, because a comment only reaches someone already
reading that file, which excludes the person writing the next copy somewhere
else.
