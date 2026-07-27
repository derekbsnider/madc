# Fix What You Find

- A defect you discover while doing something else is YOURS. There is no other
  maintainer to hand it to.
- "Pre-existing" is not a disposition. It answers a question nobody asked.
- Do not spend effort establishing that a bug predates your change unless the
  answer changes WHAT YOU FIX. Provenance is a debugging tool, never an exit.
- Never record a defect and move on when you could fix it in the session that
  found it. A filed `Gap` with no owner and no date is a shim written in prose.
- A SILENT wrong answer outranks whatever you were doing when you found it.
  Exit 0 with the wrong value is worse than a crash.
- Fix it in its OWN commit — never folded into the unrelated change that
  uncovered it, and never used to justify skipping that change's gates.
- Every such fix ships a reducer in `tests/` with an oracle (`g++` and
  `clang++` output). The test is the gate; without it the fix is a claim.
- If it is genuinely too large for the current session, say so out loud with
  the layer chain and the reducer, and fix it NEXT — not "eventually".
- When a fix reveals another defect one layer down, that one is yours too.
  Follow it down until you reach something you have actually fixed.

See `docs/rules/fix-what-you-find.md` for the reasoning.
