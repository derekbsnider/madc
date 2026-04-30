# Testing Rules

## Integration Tests

- Integration tests live in `tests/*.mad`
- Run via `make -C src fulltest` (drives `scripts/run_tests.sh`)
- All tests must pass before merging to `develop`
- Per-test fixtures use the filename convention in
  `.claude/rules/test-fixtures.md` — do not add test-specific logic
  to the runner
- Current counts live in `docs/test-status.md` and `README.md`, never
  hard-coded in a rules file

## Unit Tests

- Use **doctest** (single-header at `include/doctest.h`)
- Unit test files go in `tests/unit/`
- Run with `make -C src test`
- Each test file must:
  - `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` in exactly ONE file per binary
  - Define `bool madc_verbose = false;` and the DBG macro before including project headers
  - NOT define `dd*` global instances (they come from `parser.o` via `TESTOBJ`)
- Inside `CHECK(...)`: no bitwise AND — use `CHECK((flags & vfFOO) != 0)`
- Use `CHECK_THROWS(...)` for expected exceptions

## PR Requirements — before opening a PR to `develop`

1. `make -C src clean && make -C src` — must build without errors
2. `make -C src fulltest` — all integration + unit tests must pass
3. No new compiler warnings introduced

See `docs/rules/testing.md` for the reasoning and common pitfalls.
