# Testing madc

## Integration Tests

The `tests/` directory contains `.mad` programs that exercise the language end-to-end.

Run a single test:
```bash
bin/madc tests/testint.mad
```

Run all tests and report pass/fail:
```bash
for t in tests/*.mad; do
    out=$(timeout 5 bin/madc "$t" 2>/dev/null)
    ec=$?
    if   [ $ec -eq   0 ]; then echo "PASS:    $t"
    elif [ $ec -eq 124 ]; then echo "TIMEOUT: $t"
    else                       echo "FAIL($ec): $t"
    fi
done
```

Current status: **24/24 pass** (see `docs/test-status.md` for details).

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
