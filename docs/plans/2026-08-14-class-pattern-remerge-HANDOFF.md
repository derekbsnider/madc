# HANDOFF — re-land the lost class-pattern optimization (2026-08-14)

**Read this fully before acting. Assume a cold start.** Run
`bash scripts/resume.sh` first — it prints live git state and any orphaned
background jobs, which a compaction summary cannot.

**This is the top-priority mission.** Everything else (T6 `dirent`, Windows
W3–W5, task #44) waits behind it.

---

## 1. Why this exists — read this part twice

`feature/class-pattern-nontype-claude` holds **17 commits / ~1234 lines** of
**validated, complete** work that was **never merged**. The owner's words,
2026-08-14:

> *"I TESTED THE RESULTING BINARY, THEN YOU DIDN'T MERGE THE IMPROVEMENT, AND
> THEN THE NEXT BINARY WAS SLOWER THAN THE UNMERGED VERSION."*

> *"what is especially annoying about this was that I asked you to find out why
> things got slower and you still didn't see the unmerged code."*

**The owner personally measured the binary built from this branch and it was
faster.** The improvement never landed, so **every binary since has been
slower than the one the owner held**. This is the answer to the owner asking,
repeatedly, why things seemed slower.

### Two things NOT to do

- **DO NOT open a regression hunt.** Nothing regressed. A measured improvement
  never landed. A previous session searched `develop` history for a regression,
  found nothing (correctly — every commit was innocent), and missed this
  entirely because a history search is structurally blind to code that never
  merged. It surfaced only as a side effect of branch housekeeping.
- **DO NOT overwrite or delete `feature/class-pattern-nontype-claude`.** It is
  the artifact the owner tested. It stays pristine as reference and fallback.
  Work on a NEW branch.

Owner direction on approach: *"let's try to do a good job of it, as safely as
we can."* Prefer slow and provable over fast.

---

## 2. SETTLED — do not re-derive or re-litigate

- **The work is green on its own terms.** The branch's own `claude_status`
  records SLICE N1 COMPLETE @`51af2af9` (fulltest 1005/0, libcxxjit 1001/0,
  packed 1005/0) and SLICE N3 COMPLETE @`cd4f2d74` + `7f7fce25` (fulltest
  1007/0 + units, libcxxjit 1003/0, packed 1007/0).
- **EXE and OBJ were NEVER RUN**, on either slice — both say "EXE/OBJ at
  session end" and that session end never came. Those legs are FIRST-TIME
  validation, not re-validation. Budget for real failures there.
- **What it optimizes:** re-parse elimination. Its own commit bodies:
  *"enable_if-shaped templates (value params unused in the body) were
  **re-parsed per instantiation** solely because of the blanket non-type
  dooms"*, and the burn-down *"testsubscript packed tally: **242 → 189 body
  parses**"*. Oracle recorded on the branch: *g++ -O0 on `tests/testsubscript.mad`
  ~325ms warm vs madc-release ~425ms — "the gcc-parity pathology this campaign
  burns"*.
- **It is task #25's campaign** (packed-lane latency), which is still
  `in_progress`. Tasks #28 (slice N1) and #29 (slice N3) are marked *completed*
  and their code is on the shelf — that mismatch is the original sin here.
- **Base:** `develop` @ **v0.74.0**. `develop` is now **v0.80.0** — six releases
  later. Last branch commit 2026-08-09.

---

## 3. What is on the branch

```
d88421c8 Record slice N3 completion in the live handoff
7f7fce25 Class-pattern slice N3b: static data members capture and serve
4d04ea37 Intern the filename in Program::tokenize(const char*) — dangling token files
cd4f2d74 Class-pattern slice N3a: serve substitutes VALUE params in ValueArg runs
1cf7bbeb Bank the N3 value-param substitution requirement
8cb68715 Complete the N3 recon: capture-fail arm is the token-run capture point
769c6e7f Bank the N3 static-values trap
a1645ee3 Record slice N1 completion in the live handoff
288ad000 remote_build.sh: expand the battery shortcut anywhere in the stage list
51af2af9 Class-pattern slice N1: ValueArg encodes non-type args in dependency slots
2b6949a6 docs: N1 storage decision — value_arg_tokens side table on ClassPattern
1814453c docs: N1 serve design decided
1013f2b8 docs: N1 recon banked
ea362bca Merge develop (v0.74.0 non-const-lref fix) into the campaign
adff3811 docs: live_handoff — use-side wall campaign
f9ff4e2f Admit non-type-param templates to the class-pattern lane (slice N2)
feea1a92 Plan the class-pattern value-args campaign + keep the counter probes
```

| file | delta | note |
|---|---|---|
| `src/parser.cpp` | **+806** | THE conflict surface — see §4 |
| `include/madc.h` | +51 | `ClassPattern::value_arg_tokens` side table |
| `src/madc_cir.cpp` | +18 | |
| `src/cir_freeze.h` | +6 | freeze payload v5 → v6 |
| `src/lexer.cpp` | +9 | filename interning (dangling token files) |
| `tests/testclasspatternvaluearg.{mad,expect}` | new | **reducer / functional gate** |
| `tests/testclasspatternstatic.{mad,expect}` | new | **reducer / functional gate** |
| `tests/unit/test_class_pattern.cpp` | +79 | |
| `docs/plans/2026-08-08-class-pattern-value-args-plan.md` | +287 | the campaign plan |
| `scripts/remote_build.sh` | +14 | **conflicts** — see §4 |
| `claude_status.json` | — | **discard, take develop's** |

Also rides along: two fix-what-you-find fixes (`sizeof(placeholder)` baked 0 →
`query_datadef_measure` now throws).

---

## 4. Hazards, most dangerous first

1. **FOREST FORMAT VERSION — the merged code MUST become 40.**
   Branch is at `CIR_FOREST_FORMAT_VERSION = 38`; `develop` is at **39**. Both
   sides bumped independently. The pin is absolute (a container whose version
   differs is rejected wholesale and live parse takes over), so a *mismatch*
   fails safe. The danger is the opposite: **keeping 39** would let a reader
   accept a v39 container that lacks the branch's `value_arg_tokens` and
   statics side tables. Bump to **40**. Do not accept either side's number.

2. **`parser.cpp` has been rewritten under the branch.** Since v0.74.0 it took
   the 46b LLP64 target-width work, the class-nested enum scope fixes
   (`08a76f43`, `eb3cf5dd`), and the typeid operand hoists (`87efb272`). Expect
   real conflicts, not textual ones.

3. **`-Werror` is now on** (`63f008ad`) and this code has never been compiled
   under it. Expect cleanup. Fix what the warning reports; never `-Wno-`, never
   `WERROR=0` to land it (owner law: zero warnings, both surfaces).

4. **`remote_build.sh` conflicts.** The branch adds "expand the battery
   shortcut anywhere in the stage list"; develop gained `win`, `wine` and
   `warnscan` stages on 2026-08-14. Reconcile — the branch's change is still a
   good idea, it just has to sit beside the new stages.

5. **EXE/OBJ are unvalidated** (see §2). Treat failures there as expected work.

---

## 5. The plan, in order

**Step 0 — MEASURE FIRST, before touching anything.** Build the ORIGINAL
branch binary and current `develop`, and time both on `tests/testsubscript.mad`
via the tracked timing TSV and `scripts/perf_vs_gcc.sh`. Use the packed
(`bin/madc-release`) artifact — that is what the owner measures. Record both
numbers in this file. **This is the acceptance test**; without it "did the
speed come back?" cannot be answered, and answering it is the entire point.
Wall-clock feel is not evidence on this host.

**Step 1 — new branch, develop pulled IN.**
`git checkout -b feature/class-pattern-remerge-claude origin/feature/class-pattern-nontype-claude`
then `git merge develop`. Conflict resolution happens on the new branch;
`develop` stays clean until the result is green. The original branch is
untouched.

**Step 2 — resolve, with the reducers as the gate.**
`testclasspatternvaluearg` and `testclasspatternstatic` are the semantic proof
that the merge preserved the behaviour. They must pass. Bump the forest
version to 40 (§4.1). Take develop's `claude_status.json`. Reconcile
`remote_build.sh`.

**Step 3 — build clean under `-Werror`,** every lane
(`bash scripts/warn_scan_lanes.sh`).

**Step 4 — full battery.** `remote_build.sh fulltest exe obj libcxx release
packed`, plus the wine lane (`remote_build.sh win wine`). EXE/OBJ get real
scrutiny — first run ever for this code.

**Step 5 — prove the win.** Re-measure exactly as in Step 0 and compare against
the recorded numbers. If the improvement did not come back, STOP and say so
plainly — a merged-but-slow result means a hunk was lost in resolution, and
that is a finding, not a footnote.

**Step 6 — merge to `develop` WITH a release** (standing owner rule), then
delete the two branches and update task #25/#28/#29.

---

## 6. OPEN — needs the owner, do not decide alone

If a `parser.cpp` conflict is genuinely irreconcilable — the surrounding code
rewritten out from under a hunk — the owner was asked whether to reimplement
that hunk against the current shape (with the reducer as proof) or to stop and
show the conflict first. **The question was put but not answered before the
compaction.** Default to reimplementing with the reducer as proof, but on 806
lines that is a lot of judgement applied unseen — so surface each such hunk
explicitly in the session log, and if there are more than a handful, stop and
ask.

---

## 7. The lesson this cost

**A task marked COMPLETE is not complete until it is MERGED.** Nothing
reconciled the task list against `develop` for five days; the branch's own
status file said "complete" and was telling the truth about its own gates while
being invisible to `develop`. Verify completion claims against `develop`, never
against a branch status file.

**And: for any "worse than before" report, run
`git branch -r --no-merged develop` and read what each branch claims BEFORE
profiling or bisecting.** Ask the owner which artifact they measured — the
baseline may not be on the mainline at all. See KG
`Gap{class_pattern_nontype_branch_unmerged}` and the memories
`project_class_pattern_unmerged` / `feedback_check_unmerged_when_worse`.

---

## 8. Also unmerged, much smaller — do it alongside

`feature/windows-release-lane-claude` (docs only, 2 commits). Cherry-pick its
**+50 lines** to `docs/plans/2026-08-12-windows-release-lane.md`: the 46b
landing record, which carries a process lesson worth keeping —
*a win64 MODEL change requires the FULL wine suite in the gate, never a subset;
a 17-test subset was green while 115 tests were broken.* The 406-line Codex
handoff doc is historical (46b shipped in v0.79.0) and its `claude_status.json`
edits are superseded. Then that branch can be deleted.
