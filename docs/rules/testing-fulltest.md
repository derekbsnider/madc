# Testing Fulltest — Reasoning and Gotchas

See `.claude/rules/testing-fulltest.md` for the rule itself.

## Why `make -C src fulltest` stays the default gate

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
