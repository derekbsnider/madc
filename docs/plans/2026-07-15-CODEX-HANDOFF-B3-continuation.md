# CODEX HANDOFF — B3 continuation (fresh session; predecessor hit its usage wall mid-slice)

**From:** Claude, 2026-07-15. **For:** a NEW Codex session.
**Branch:** `feature/class-parse-once-codex` @5ad4efc6 (pushed).
**Parent brief:** `docs/plans/2026-07-14-CODEX-HANDOFF-class-parse-once-B0-B6.md` —
still the authoritative ladder (slices, gates, traps, SETTLED). This doc is the
continuation STATE + the priorities it changes. The design spec remains
`docs/plans/2026-07-14-class-kind-parse-once-DESIGN.md` (§15 carries the owner
approvals).

## WHAT HAPPENED

The predecessor session committed B0 (`c6f05b48`), B1 (`05fda7a5`), B2
(`3e99b9d4`), then ran out of usage MID-B3 without committing or writing a
handback — it kept coding when asked to wrap up. Claude salvaged the working
tree verbatim as `5ad4efc6` ("WIP: B3 vector closure", 2,677 insertions across
parser.cpp / madc.h / madc_cir.cpp / cir_freeze.h / intern_table.h + expanded
unit tests + new tests/testclasspatternvector.mad). The salvage is UNREVIEWED —
read the `5ad4efc6` diff before building on it.

**Session discipline (owner directive, learn from this):** when the owner says
wrap up, STOP CODING immediately — commit the WIP, push, write the handback.
An uncommitted 2,700-line tree is one `git checkout` from oblivion.

## VERIFIED STATE (Claude, 2026-07-15, on the salvaged tree)

- Builds clean: `make -C src` rc=0, ZERO warnings.
- **The salvaged state is exactly the state the predecessor's final fulltest
  validated**: `tmp/b3-fulltest-final.log` (17:43) postdates the last source
  edit (17:23) → **697 passed / 0 failed / 0 timed out / 16 skipped** (the +2
  are the new classpattern tests). All doctest suites green inside it.
- Spot-verified live: `tests/testclasspatternvector.mad` output matches
  `.expect`; `testclasspatternbasic` passes.
- Counters are live and the pattern lane ENGAGES (live testsubscript):
  `class instantiate . 134 pattern / 300 parse / 690 cache / 153 opaque`.

## 🔴 PRIORITY 1 — the B3 acceptance gate is currently FAILING on time

B3's acceptance is "BOTH live and bound instantiate + total wall DECREASE."
Right now the WIP is slower live, not faster:

- Live dev (-O0) testsubscript in-process total **3.78 s** (pre-ladder dev
  baseline ≈ 2.7 s; TSV live medians 2.17–2.42 s). Bench row recorded by the
  predecessor: live 9.624 s / bound 0.999 s at load 3.07 — confounded (load>2)
  but directionally consistent.
- `instantiate` bucket: 1.390 s, **8,664 calls vs the 6,801-call live
  baseline (+1,863 calls)** — the machinery is doing MORE instantiation work,
  not less.

Attribute BEFORE widening coverage. Hypotheses to test (measure, don't guess):
1. The pattern lane triggers instantiations the old path never made (the
   +1,863 calls — where do they come from? counter the call sites).
2. Capture/normalization work is running per-instantiation instead of once per
   pattern.
3. `pattern-parse-error` fallbacks (see profile below) pay for a failed capture
   attempt AND the full parse — double work on every hit.
4. Something in the 2,180-line parser.cpp diff regressed the non-template path.

Method: A/B against B2 (`3e99b9d4`) with the counters + callgrind per-site
call counts (bound runs complete under the ~120 s CPU kill; live -O0 runs
truncate — compare sites, never totals). The B2 build is one
`git checkout 3e99b9d4` away — commit/stash NOTHING loosely; the branch tip is
the salvage, keep it intact.

## PRIORITY 2 — the packed leg was NEVER run on this WIP

The predecessor validated live only. Required before B3 can close:
`make -C src release` → blob-presence check → packed suite
(`MADC_BIN=bin/madc-release bash scripts/run_tests.sh`, expect 697/0/0/16) →
bind gate. REMEMBER: B1 bumped `CIR_FOREST_FORMAT_VERSION` — every pre-B1
`.msnap` corpus is stale (version-rejects to live parse); re-freeze anything
you probe with (`bin/madc --freeze=tmp/<x>.msnap tmp/_bf3.cpp`).

## PRIORITY 3 — finish B3 coverage (after 1 & 2 are green)

300 class instantiations still take the parse lane on testsubscript. The
ranked profile names the families (live capture, 2026-07-15):

```
1. 18 x std::__and_            [why: pattern-parse-error]
2. 18 x std::__conditional     [why: dependent-value-expression]
3. 18 x std::reverse_iterator  [why: unnormalizable-type]
4. 14 x __gnu_cxx::__alloc_traits    [why: pattern-parse-error]
5. 14 x std::__iterator_traits [why: pattern-parse-error]
6. 12 x std::__new_allocator   [why: unnormalizable-type]
7. 12 x std::allocator         [why: unnormalizable-type]
8. 10 x __gnu_cxx::__normal_iterator [why: unnormalizable-type]
9. 10 x std::allocator_traits  [why: unnormalizable-type]
10. 9 x std::_PCC              [why: dependent-value-expression]
11. 9 x std::__pair_base       [why: unsupported-friend-definition]
12. 9 x std::pair              [why: dependent-value-expression]
```

Note `pattern-parse-error` is not an eligibility class — it smells like CAPTURE
defects (the pattern failed to parse/normalize where it should have). Fix the
capture at the deepest layer; do not widen an eligibility predicate around a
bug. The others are genuine KIND gaps (dependent value expressions, friend
definitions, unnormalizable types) — widen per-KIND per the design, ratchet
green each step.

## THEN: B4–B6 per the parent brief

Unchanged: string closure, map/set/list closure, burn-down to zero; handback
checkpoints at B3 and B6 (B1's checkpoint already passed — its round-trip
landed in `05fda7a5`; Claude will independently verify B3 before it merges).
All gates, traps, and SETTLED sections of the parent brief apply verbatim —
especially: packed suite is the arbiter; blob presence with every packed
result; no name-keyed exceptions; never `git add -A`
(foreign `mir-debug-support.md` at repo root); no `&&` chains; builds in the
background; scratch in `tmp/`.

## ACCEPTANCE FOR THIS SESSION

1. The live-time regression attributed with numbers, then fixed or the
   offending mechanism redesigned — live testsubscript back at or below the
   2.4 s dev-median band, instantiate calls back at or below baseline.
2. Packed leg green on the WIP (697/0/0/16 + blob + bind gate).
3. B3 closed per its original gate: the 7-specialization vector-chain parse
   count materially in the pattern lane, BOTH live and bound walls improved,
   bench row at load<2, binary size unchanged.
4. A handback note at the B3 checkpoint BEFORE starting B4 — and if usage runs
   low at any point: commit, push, handback. Wrapping up beats one more edit.
