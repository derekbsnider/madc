# Fix What You Find — the reasoning

## The incident

On 2026-07-27, while migrating the last hand-rolled delimiter tracker, probing
turned up four defects that had nothing to do with the migration. For each one
the response was the same: build a discriminator proving the defect was
**pre-existing**, write it into the knowledge graph with a reducer and a layer
chain, and move on.

The owner's correction:

> *"'not mine' is never acceptable — you've been working on this code for
> months… any discovered bug is yours to fix, who else is going to fix it?"*

That is the whole argument, and it is unanswerable. **"Pre-existing" is only a
meaningful category when there is somebody to hand the defect to.** In a
codebase with one maintainer it decodes to "I broke this on a different day",
which has no bearing on whose job it is.

## Why it felt like diligence

This is the part worth remembering, because the failure did not feel like
laziness — it felt like rigour. Each filing had a real reducer, a verified
`g++`/`clang++` oracle, a named layer, and a control proving the scanner under
repair was innocent. It read as careful engineering.

The measurable tell: **the effort spent proving provenance was comparable to
what the fixes actually cost.** The immediately-invoked-lambda defect — three
different wrong behaviours across argument position, assignment and `auto`
deduction, one of them silent — took three edits once the question changed from
*"is this mine?"* to *"where is the dispatch?"*.

Any time the investigation budget is going into attribution rather than the
layer chain, the wrong question is being asked.

## Silent wrong answers jump the queue

Of the defects found that day, the one that mattered most produced **exit 0 and
the wrong value**:

```cpp
printf("%d", [](int x){ return x + 99; }(1));   // g++: 100    madc: 1
struct Foo { P p; Foo() : p{1,2} { } };         // g++: 1 2    madc: garbage
```

A crash is self-reporting; a wrong value is not. It propagates into whatever
consumes it, and the eventual bug report points somewhere else entirely — the
same property that let the angle-bracket defect survive seven weeks pointing six
headers away from its cause.

So a silent wrong answer outranks the task in hand. Not "file it and continue" —
**stop and fix it**, then resume.

## Follow it down

Fixing the aggregate member initializer surfaced the reason it was silent:

```cpp
if (ci->args.size() != 1 || !ci->args[0])
        continue;                 // member never initialized, nothing said
```

A `continue` for input the code does not understand. That is the same defect
shape as the parse-side loop that accepted any sequence of expressions without
requiring a separator. **When a fix uncovers another defect one layer down, that
one is yours as well** — otherwise the session ends with a fix sitting on top of
the thing that made the bug invisible.

## The gate, and its honest limit

The mechanism here is weaker than a ratchet, and saying so keeps it from being
mistaken for one. There is no way to mechanically detect "you noticed something
and walked past it".

What is enforceable, and what this rule requires:

- **Every such fix ships a reducer in `tests/`.** `fulltest` then owns it
  forever. A fix without a test is a claim; a test is the only artifact that
  survives the session's confidence.
- **Its own commit**, so the fix is not laundered through an unrelated change
  and does not inherit that change's justification.
- **The `Oracle:` trailer** already required by `check-rule-trailers.sh` forces
  the `g++`/`clang++` comparison into the permanent record.

Deliberately absent: a "known-failing test" lane. Adding one would build
machinery for deferral, which is the behaviour this rule exists to stop. If a
defect is genuinely too large for the session, it is stated out loud with its
layer chain and fixed next — not parked in a lane that makes parking
comfortable.

See `.claude/rules/fix-what-you-find.md` for the bare rules.
