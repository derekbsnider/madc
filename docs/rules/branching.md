# Branching Model — Why

Feature branches isolate new work from the stable `develop` branch. This was adopted after discovering that uncommitted changes across multiple files are fragile — a single bad `git checkout` can destroy everything.

The workflow: `feature/name` branch off `develop`, commit early and often, merge back via `--no-ff` when stable. This keeps the git history clean and makes it easy to compare feature vs baseline.

`develop` is the integration branch — all tests must pass. `master` is for releases only.

Now that work is handed back and forth between Claude Code and Codex CLI,
agent-owned feature branches reduce branch collisions and make unfinished work
safe to park. Suffixing branches with `-claude` or `-codex` makes ownership
obvious in `git branch` output and in hand-off notes:

- `feature/hashstr-next-claude`
- `feature/hashstr-next-codex`

The point is not permanent silos. It is to make WIP explicit. When one agent
hands a branch to the other, that hand-off should be recorded in the session
note and in the updated status files so both tools agree about who owns the
next edit.

## Why the promote gate is "all class-(a) green", not "match asmjit's 1645"

The original gate said develop must match or exceed master's asmjit-backend
torture count (1645/1685). The 2026-06-11 failset classification audit
(`docs/parity/failset-classification.md`, user-signed) showed that number no
longer measures the right thing:

- The two backends' capability sets diverged — CIR passes 34 tests asmjit
  fails, asmjit passes 82 CIR fails — so comparing raw counts compares
  different feature sets, not progress toward correctness.
- Classifying every remaining failure found 33 tests that exist only to
  exercise gcc internals, gcc-only extensions with no real-world use, or
  undefined behavior where madc's choice is as defensible as gcc's (clang
  diverges from gcc on several of them too). Chasing those is parity
  theater, and the user ruled 100% torture parity is explicitly not the
  goal.
- What actually gates promotion is standard-C correctness: the 41 class-(a)
  failures are real compliance bugs (dominated by K&R old-style definitions
  and implicit-decl handling — exactly the C89 SMAUG-class material madc
  exists for). Hence: gate = all class-(a) fixed = ≥1608 of the 1652
  in-scope tests, class-(c) formally skipped, class-(b) on the roadmap.

## Why the lane-freshness push gate (2026-08-28)

- Three lanes drifted silently in one month: SMAUG was red five weeks in
  every shipped release (no lane compiled the artifact), the wine lane
  accumulated 75 red tests from a July 24 emission change (the lane last
  ran ~v0.92.1), and macOS went five releases with nothing darwin even
  compiled (the MT arc's rt_task.c would not build there at first try).
- The common failure: "promote tests everything" was a checklist, not a
  mechanism — a rule without a gate decays. The ledger makes the claim
  mechanical: a lane is FRESH iff no code content (src/ include/
  third_party/ tests/ scripts/ tools/ examples/) changed since its
  recorded green commit, so docs-only commits never stale a lane and any
  code change stales every lane that has not re-run.
- The gate rides the PUSH of develop/master (owner directive), not just
  /promote, because develop is the integration branch other work builds
  on — a red lane behind a pushed develop propagates.
- The check runs its own negative control first (a stale row must block,
  a fresh row must pass): a gate that cannot fail is not a gate. The
  selftest is IN-PROCESS — its first version re-entered the script via
  "$0" and fork-spiraled the QNAP into a hard reset (2026-08-28); the
  tombstone comment in lane_ledger.sh marks the lesson.

## Why every platform lane's FULL suite gates master (owner law 2026-09-04)

- The first ledger gated what ran; it did not ask whether what ran was a
  TEST of the platform. The `macos` row was the container cross build plus
  pack verification — a build lane — and the CI "Mac battery" is a ten-test
  smoke. The full darwin suite ran only when the owner dispatched
  darwin-probe.yml by hand, and its residue (9 aarch64 MIR-floor tests, 3
  libc++ `<list>` tests) lived in a plan document, not in the gate.
- The libc++ flavor lane (macOS's library, run on linux hardware) was not in
  the ledger at all. Its last full run was 2026-08-16 (1046/0/13skip); the four
  std::list tests landed 2026-08-18; the three libc++ `<list>` failures were
  visible only on the darwin runner. Nothing on linux had exercised libc++
  `<list>` under madc, and the lane's green was remembered as covering it.
- genuine Windows was `promote=no` ("flip when automatable") — an opt-out
  by omission from the one gate that exists.
- The ledger header said "/promote runs it as a hard gate"; the /promote
  command did not. A checklist again, in a comment.
- The mechanism: a second check mode. `check --promote` (the develop push)
  gates `promote=yes` rows; `check --release` (the master push and /promote)
  gates `yes` AND `release` rows. The release tier holds the platform suites
  whose cost or hardware keep them off every develop push: `libcxx`,
  `darwin-suite` (darwin-probe with `suite_gate=true`, both arches),
  `genuine-win`. An unrecorded row (`none`) is stale by definition, so master
  is blocked until each has run green on the candidate. The selftest carries
  the third negative control: a stale release-tier row must pass `--promote`
  and block `--release`.
- Consequence stated plainly: the darwin residue is a promotion blocker. Each
  failing test is fixed, or skipped with a `.darwin_skip` whose one line says
  why it is structurally out of the domain — the class-(c) convention of the C
  torture gate — before a master release.
