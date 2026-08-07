# Rule Trailers — the reasoning

## Why this exists

On 2026-07-27 the owner asked: *"isn't there some way to ensure that you follow
at the very least, the top 5 rules?"* The honest answer was no — not by adding
rule text, because the rules were **already written down and were violated
anyway, repeatedly, in the same session where they were being cited.**

What actually caught mistakes that day was never a rule. It was a mechanism:

| Mechanism | What it caught |
|---|---|
| `check-one-delim-tracker.sh` ratchet | an off-by-one baseline set from a hand count |
| the same gate's first run | the duplication family was 27 sites, not 8 |
| `fulltest` | a strictness regression on `operator _Tp()` |
| the stated-search requirement in Rule #4 | two would-be duplicate helpers |
| **nothing** | Rule #2 — the owner had to catch that one by hand |

The pattern is exact: **a rule holds when skipping it leaves a visible hole.**
Rule text leaves no trace when ignored, which is why "read the rules more
carefully" has never worked and will never work.

## The design constraint

A gate that mechanically verifies *"was this the deepest layer?"* is impossible
— it requires understanding the program. So the gate verifies the **artifact**
instead: that the reasoning was written down, in the permanent record, where a
reviewer can check it cheaply and argue with it later.

That is strictly weaker than `check-one-delim-tracker.sh`, which mechanically
counts real code. It is stated here plainly so nobody mistakes the two:

- **Mechanical gates** (delim tracker, `no-std-hardcoding`, `tsubst` ratchet,
  and `remote_build.sh`'s busy-check) verify facts about the tree. They cannot
  be talked around.
- **Artifact gates** (this one) verify that a claim was made. A determined
  author can write a plausible-but-wrong `Layer:` line. What they cannot do is
  write *nothing* — and that is the failure mode being closed, because silent
  skipping was the actual observed behaviour.

## Why `Layer:` is the important one

Rule #2 failed in a specific and instructive way. The shim was not laziness; it
came with a **genuine architectural argument** — "a scanner needs the *extent*
of a name, a parser needs its *validity*, so they legitimately differ." That
sentence is true. It is also exactly what made the shim invisible: a plausible
principle is the most effective camouflage a shortcut can wear.

Writing the chain out defeats that, because the honest version reads:

```
Layer: skipper -> delimStepStream -> parseOperatorId cannot read a
       conversion-function-id  <- DEEPEST.  I am editing delimStepStream.
```

…which is self-evidently wrong on the page. The requirement is not that the
author be more virtuous; it is that the shim become **legible**.

## Why `n/a — reason` is allowed

Because a gate that cannot be satisfied honestly gets disabled, and a disabled
gate protects nothing. Most commits genuinely have no oracle — a
behaviour-preserving refactor's oracle *is* the suite. Forcing `n/a — reason`
rather than permitting silence keeps the judgement in the record, where it can
be disagreed with in review, instead of in someone's head.

## The companion mechanical guard

`scripts/remote_build.sh` now **refuses** to sync, build, or launch a suite while
the container already has one running. That closes a different 2026-07-27
failure: a relink landed under a live suite, then a second suite started beside
it, breaking "never rebuild mid-suite" and "one heavy container job at a time"
in the same stretch. Both runs reported `rc=0`. Both were discarded.

That one is worth studying because **the damage read as green.** Sequencing
hazards are invisible in the moment — each individual command looks fine, and
only the ordering is wrong. No amount of care at the point of typing catches
that; a refusal does. `MADC_ALLOW_CONCURRENT=1` overrides it, and the override
is deliberately loud.

## Maintenance

`EPOCH` in the script moves **forward only**. Moving it backward, or forward
past a commit you did not want to justify, converts the gate into decoration —
which is the failure mode the whole file exists to prevent.

See `.claude/rules/rule-trailers.md` for the bare rules.
