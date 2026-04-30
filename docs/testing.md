# Testing madc

## Integration Tests

The `tests/` directory contains `.mad` programs that exercise the language end-to-end.

Run a single test:
```bash
bin/madc tests/testint.mad
```

Run all tests via the Makefile:
```bash
make -C src fulltest
```

Current status: **83/83 pass** (see `docs/test-status.md` for details).

Under the hood, `scripts/run_tests.sh` iterates `tests/*.mad` and
discovers per-test fixtures by filename convention. For a test
`tests/foo.mad`, the runner uses (all optional):

| Fixture           | Effect                                             |
|-------------------|----------------------------------------------------|
| `tests/foo.input` | Redirected to stdin (`bin/madc foo.mad < foo.input`) |
| `tests/foo.argv`  | Whitespace-split, appended as argv                 |
| `tests/foo.expect`| Each non-empty line must appear in the output      |

The runner is deliberately generic — it does not know about any
individual test. To add stdin to an existing test, drop a `.input`
file next to it and re-run. This keeps each test's setup and
expected output co-located with the test itself (high cohesion,
low coupling to the runner).

Note: `tests/include_helper.mad` is not a standalone test — it's
included by `testinclude.mad`. The runner skips it by name.

## Unit Tests

Unit tests use [doctest](https://github.com/doctest/doctest) (single-header,
in `include/doctest.h`). Test files live in `tests/unit/`.

Build and run:
```bash
make -C src test
```

This compiles each `tests/unit/*.cpp` against the madc object files and runs them.

### Current Unit Test Files

| File | Tests | Covers |
|------|-------|--------|
| `tests/unit/test_datadef.cpp` | 25 | DataType enum, varflag_t, DataDef type queries, DataDefSTRUCT layout, Variable set/get/cmp |

### Writing a New Unit Test

Create `tests/unit/test_<name>.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <asmjit/x86.h>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
// include "madc.h" if you need Program/TokenCpnd etc.

TEST_SUITE("My component") {
    TEST_CASE("something works") {
        CHECK(1 + 1 == 2);
    }
}
```

The Makefile's `test` target automatically picks up all `tests/unit/*.cpp` files.

**Notes:**
- Do not define the `dd*` globals (ddINT, ddSTRING, etc.) in test files — they come from the linked `parser.o`.
- Use `CHECK((flags & vfSOMEFLAG) != 0)` rather than `CHECK(flags & vfSOMEFLAG)` — doctest forbids bitwise AND in CHECK expressions.

## Verbose Debugging

When a test fails unexpectedly, run with `-v` to see trace output:

```bash
bin/madc -v tests/failing_test.mad 2>&1 | head -50
```

This shows the tokenizer output, parse steps, and JIT compilation decisions.
