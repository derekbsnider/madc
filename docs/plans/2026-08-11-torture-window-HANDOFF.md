# HANDOFF — the 5-fail torture window (task #41) — session #82 close, 2026-08-11

**RESOLVED (session #83, 2026-08-12).** All five fixed on
`feature/torture-window-t41-claude` — four root defects, each its own
commit with an oracle-backed reducer in `tests/`:

| Fix | Commit | Tests it greens |
|-----|--------|-----------------|
| `DataDefLDOUBLE::alignment()` = 16 (SysV; the override its own comment promised) | 533aeae5 | 20040709-1/2/3 (with ↓) |
| `__builtin_classify_type` = parser builtin folding gcc's typeclass (was a macro → 0) | ac1c583f | 20040709-1/2/3 |
| Anonymous aggregate inlines its body even after nested-type class promotion | 2bb2dda1 | 20000717-4 |
| Nested brace list = aggregate recursion, not a ctor arg; plain-struct sub-braces typed from the member | 71d83a21 | pr39339 |

The §"UNVERIFIED hypothesis" below is DISPROVEN — the #74
unprototyped-call change was innocent. The window's 2026-07-27
type-correctness work (`114b13a8` long double became a real 16-byte
type; `6fec105d` nested types promote the enclosing struct) unmasked
three latent defects, and `8f8f4009` (2026-08-06) turned the fourth
from silent init-drop into the loud shape error. All five were
standard-C class-(a) — the promote gate WAS retroactively blocked and
this batch unblocks it. Validation and the restored-baseline stamp:
`docs/parity/failset-classification.md` (2026-08-12 paragraph) +
CHANGELOG [Unreleased]. The section below is the historical mission
brief.

Read FULLY after compaction (assume cold start). **The owner's directive:
resolve these 5 torture regressions NEXT** — before the Windows release
plan (which follows them, and the GitHub-runner work follows Windows).

## THE MISSION

Five gcc.c-torture tests regressed somewhere in the 2026-07-23 →
2026-08-11 window. Reduce, attribute to a commit, classify, fix.

| Test | Failure |
|------|---------|
| `20000717-4.c` | runtime, exit 1 |
| `20040709-1.c` | runtime, madc diagnostic |
| `20040709-2.c` | runtime, madc diagnostic |
| `20040709-3.c` | runtime, madc diagnostic |
| `pr39339.c`    | compile, madc diagnostic |

- Files: `/workspace/gcc_testsuite/gcc.c-torture/execute/` (CONTAINER —
  QNAP never builds/tests).
- Run: `python3 scripts/run_gcc_testsuite.py --root /workspace/gcc_testsuite
  [--madc bin/madc] [test paths...]` (defaults `--std=c17`, 5s timeout).
- Baseline being restored: **1614/1/9/0/61** (@9a48a7fc, 2026-07-23),
  failset = exactly the 10 classified class-(b) names
  (`docs/parity/failset-classification.md`, incl. the 2026-08-11 update
  paragraph recording this window).

## EVIDENCE ALREADY ESTABLISHED (do not re-derive)

- **NOT the MIR migration**: all 5 fail identically at pre-migration
  v0.76.0 (@16e04001, built against standalone `/workspace/mir`) —
  attribution run recorded in the #82 transcript. Migration torture was
  byte-identical pre/post.
- **Window**: @9a48a7fc (last green sweep) → @16e04001. Torture never
  ran inside it. **master (v0.76.0 @11140c7a, promoted + published)
  carries these fails.**
- **UNVERIFIED hypothesis (test first — it is cheap)**: the session-#74
  unprototyped-call proto change — fork commits `e2c0ae95` + `731c2234`
  ("an unprototyped call is not a variadic call") — the failing tests
  are K&R/implicit-decl/struct-passing era. Since MIR is now IN-TREE,
  probing this = editing `third_party/mir/c2mir/c2mir.c` like ordinary
  madc source; `git log 9a48a7fc..16e04001 -- src include` +
  `git -C /workspace/mir log` for the fork half of the window.
- **Bisect mechanics if needed**: old madc commits reference
  `MIR_COMMIT` pins; `/workspace/mir` (NAS + container) still holds the
  full fork history for checking out matching pins. Post-migration
  commits need no such pairing.
- **Classification stakes**: if ANY of the 5 is class-(a) (standard C),
  the promote gate ("zero class-(a) outstanding", ≥1608) is
  retroactively unmet — **the next /promote is BLOCKED until fixed**.
  Class-(b) GNU-extension outcomes are roadmap items, not blockers.
  20040709-* are struct-passing tests (bitfields/small structs — read
  them before assuming class); pr39339 is a compile fail (read the
  diagnostic first).

## METHOD (house rules apply)

Per test: reducer in `tmp/` → gcc AND clang oracles FIRST
(`gcc-methodology.md`/`clang-methodology.md`) → hypothesis → fix at the
deepest layer (madc parser/CIR vs `third_party/mir` c2mir — Tier
boundaries per `lowering-vs-raising.md`) → the fix ships a reducer test
in `tests/` with an oracle (fix-what-you-find) → trailers on every
src/include commit (c2mir edits under third_party/ are NOT trailer-gated
but deserve the same rigor). Per-fix targeted runs (the 5 tests + the
new reducer); fulltest+libcxxjit gate the push; torture full sweep ONCE
at the end to stamp the restored baseline in
`docs/parity/failset-classification.md`.

## STATE AT CLOSE (2026-08-11 ~23:40)

- develop @ **9827e74a** = v0.77.0 (the MIR subtree migration) +
  close-out; pushed. master = v0.76.0 @11140c7a. Working tree clean
  except owner-untracked `test.mad` / `testsort.mad` (NEVER `git add -A`).
- **MIR IS IN-TREE** (`third_party/mir`; ADR 0002): no MIR_COMMIT, no
  MIR_VERSION, no fork ceremony; `make -C src` builds libmir+c2m into
  `obj/mir/<variant>`; `mirclean` resets them; remote_build.sh has no
  mir legs; the trailer gate skips imported ancestry;
  `derekbsnider/mir` is FROZEN (historical + upstream-PR transport;
  mir#461/462/463 pend there). Do not re-litigate or re-validate the
  migration — every §25/§26 gate passed, evidence in CHANGELOG
  [v0.77.0] + claude_status + ADR 0002.
- Container synced at v0.77.0; artifacts: dist/ has verified 0.77.0
  macOS tarballs (verify green ×2 per arch after the @46afd344 gate
  fixes); `tmp/release-bins/madc-release-v0.77.0` archived both hosts.
  Mac battery on the 0.77.0 arm64 tarball: **8/2 of 10** (fails = the
  two known-opens: groves husk, darwin value SIGSEGV). Mac staging
  `~/madc-s82` (ssh derek@192.168.1.79, ⚠ LC_ALL=C in every command).
- v0.77.0 is a DEVELOP release; /promote is the owner's call and is
  soft-blocked by this task's classification outcome.

## QUEUE AFTER THIS TASK (owner sequencing, 2026-08-11)

1. **Windows release plan** (Track 6.4, marked NEXT in ROADMAP).
2. GitHub-Actions release automation — only after a working Windows
   build (design notes in the migration plan §30 + project memory).
