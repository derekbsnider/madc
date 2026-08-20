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
