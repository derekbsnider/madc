# Testing Rules

## Integration Tests

- Integration tests live in `tests/*.mad`
- Run with `bin/madc tests/<name>.mad`
- All 24 tests must pass before merging to `develop`
- Use `timeout 5` when running tests in batch to catch hangs
- Expected output is documented in `docs/test-status.md`

## Unit Tests

- Unit tests use **doctest** (single-header at `include/doctest.h`)
- Unit test files go in `tests/unit/`
- Run with `make -C src test`
- Each test file must:
  - `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` in exactly ONE file per binary
  - Define `bool madc_verbose = false;` and the DBG macro before including project headers
  - NOT define `dd*` global instances (they come from `parser.o` via `TESTOBJ`)

## doctest Gotchas

- Bitwise AND is forbidden inside `CHECK()`:
  ```cpp
  // WRONG
  CHECK(flags & vfMODIFIED);
  // CORRECT
  CHECK((flags & vfMODIFIED) != 0);
  ```
- Use `CHECK_THROWS(...)` for expected exceptions
- `TEST_SUITE("name")` groups related tests for filtering

## PR Requirements

Before opening a PR to `develop`:
1. `make -C src clean && make -C src` — must build without errors
2. All 24 `.mad` integration tests pass
3. `make -C src test` — all doctest unit tests pass
4. No new compiler warnings introduced
