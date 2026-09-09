# Testing Fulltest — Reasoning and Gotchas

See `.claude/rules/testing-fulltest.md` for the rule itself.

## Why fulltest moved from per-change to per-merge-wave (owner, 2026-08-19)

The rule originally read "fulltest after every change." In practice that
compounded into multiple full batteries plus release-lane ceremonies in a
single day (v0.89/0.90/0.91 each ran fulltest + exe + packed + headerless),
which the owner called out directly: it "wastes time and cpu and disk
grinding away with thousands of tests for every little change" — the fifth
repetition of the same feedback. The battery's job is to keep `develop`
stable at the point work MERGES, not to re-prove every incremental commit;
a targeted run (the new gates plus the touched subsystem's tests) catches
the same regressions at a fraction of the cost, and the full battery still
runs once, where it counts. The blast-radius exception exists because some
layers (lexer include machinery, shared codegen) genuinely touch every
test — there a targeted run cannot bound the risk.

## Why the merge wave is drawn at feature completion (owner, 2026-09-07)

"Battery once per merge wave" only saves time if the merge wave is a COMPLETE
feature. The push-gate battery is hours of cross-platform suites (fulltest +
exe + obj + packed + headerless, then c-testsuite, wine, and the macOS cross
build). Launch it on a feature that still has a known-open fix and the moment
that fix lands the lane ledger marks every lane stale ("code content changed")
and the whole battery runs AGAIN — the same hours twice.

The owner drew this line after a battery was kicked off with one of a feature's
three live-window bugs (the web-window resize-fill) still unbanked: "I prefer to
try to get a feature completed before running hours and hours of test suites,"
and "we established to try to bank a bunch of fixes before running the 2-hour+
long full test suites." So bank the whole feature — every slice, every
known-open fix — THEN run the long suites once.

This is a judgment rule, not a mechanical gate: nothing can detect "feature
complete" for you. The lane ledger's freshness check is the cost signal it
minimizes — every content change after a green lane re-stales it, so the
cheapest path is to make the content whole before the first expensive run.

## Why `make -C src fulltest` stays the merge-wave gate

That target is still the single command that exercises the normal unit
and integration suite the way the repo expects. It catches the common
parser/compiler/runtime regressions without relying on ad hoc command
loops or per-agent habits.

## Why native-EXE work needs an explicit second lane

`make -C src fulltest` validates the normal JIT-backed path. It does
not prove that `save_executable()` / standalone ELF execution still
works. Native-AOT bugs routinely hide behind green JIT results because
they stress different surfaces:

- ELF patching and relocation
- standalone helper export/linkage
- aggregate return ABI at real call boundaries
- startup/runtime state reconstruction
- global data materialization

So for native executable, AOT, parity, or shared codegen work, the task
is not done until `bash scripts/run_tests.sh --exe` is green too.

## Why the rule forbids leaving one lane broken

The SMAUG work made this concrete: "JIT green" was not enough. A small
aggregate return bug left the EXE lane unable to create a character
cleanly even though the ordinary suite was passing. The repo should not
accept that kind of partial validation as "good enough" anymore.
