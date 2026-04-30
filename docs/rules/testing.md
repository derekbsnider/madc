# Testing — Reasoning and Gotchas

See `.claude/rules/testing.md` for the rules themselves.

## Why `make -C src fulltest`, not a shell loop

Everything else drifts — paths, skip lists, timeout values, the
`include_helper.mad` exception. The Makefile target delegates to
`scripts/run_tests.sh`, which is the single source of truth for how
the test suite runs. Anyone running the tests by hand gets the same
result the CI / pre-commit gets.

## Why `dd*` globals come from `parser.o`

`ddINT`, `ddSTRING`, `ddVOID`, etc. are defined once in `parser.cpp`.
Unit tests link against the same objects (via `TESTOBJ` in the
Makefile), so redefining them in the test file would cause linker
errors. Tests just `extern`-reference them implicitly through the
included headers.

## Why `CHECK((flags & mask) != 0)` and not `CHECK(flags & mask)`

doctest's `CHECK` macro stringifies and analyses the expression. `&`
is ambiguous between bitwise-and and address-of inside the macro's
expression-tree parser; doctest refuses to compile it. The explicit
`!= 0` both satisfies doctest and documents intent.

## Why `CHECK_THROWS` instead of try/catch

doctest captures thrown exceptions without stopping the test run, and
reports the throw location. Hand-written try/catch swallows the stack
trace and doesn't integrate with the pass/fail totals.

## The `timeout 5` in run_tests.sh

Infinite loops in compiled test programs would otherwise hang the suite
indefinitely. 5 seconds is enough for every known test (the slowest
run in under a second on a modern machine) with a wide margin. If a
test legitimately needs longer, document why and raise the limit for
that specific test — don't bump the global.

## Why fixtures live next to tests

See `.claude/rules/test-fixtures.md` and
`docs/rules/test-fixtures.md` — same argument, different angle.
