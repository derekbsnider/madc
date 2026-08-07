# Testing madc

## Integration Tests

The `tests/` directory contains `.mad` programs that exercise the language
end-to-end.

Run a single test:
```bash
bin/madc tests/testint.mad
```

Run everything via the Makefile:
```bash
make -C src test      # unit tests (doctest)
make -C src fulltest  # unit + integration tests + every gate — THE gate for "done"
```

Current pass counts live in `docs/test-status.md` and `README.md` — they are
never hard-coded here.

### The runner and its fixtures

`scripts/run_tests.sh` iterates `tests/*.mad` and discovers per-test setup by
filename convention. For a test `tests/foo.mad` (all optional):

| Fixture | Effect |
|---------|--------|
| `tests/foo.flags` | Whitespace-split compiler flags prepended before the source path |
| `tests/foo.input` | Redirected to stdin |
| `tests/foo.argv` | Whitespace-split, appended as argv |
| `tests/foo.expect` | Each non-empty line must appear in the output |
| `tests/foo.expect_err` | Compile-error test: must exit nonzero and stderr must contain each line |
| `tests/foo.expect_quiet` | The run must produce EMPTY stderr |
| `tests/foo.timeout` | Per-test wall-clock cap in seconds (default 5) |
| `tests/foo.mir_skip` | Skip under the MIR backend (backend-floor gap) |
| `tests/foo.exe_skip` | Skip in the native-artifact passes (`--exe` / `--obj`) |
| `tests/foo.obj_skip` | Skip in the `--obj` pass only |
| `tests/foo.libcxx_skip` | Skip in the `-stdlib=libc++` flavored lane (content = the documented reason) |

The runner is deliberately generic — it knows no individual test by name.
To give a test stdin, drop a `.input` file next to it. When the runner needs
a new capability, it gets a new filename convention, never a per-test branch.
The full rules are in `.claude/rules/test-fixtures.md`.

Every test invocation runs under both a wall-clock `timeout` and a
`ulimit -t` CPU cap, so a hang fails the suite instead of pegging the host.

Note: `tests/include_helper.mad` is not a standalone test — it is included by
`testinclude.mad`.

### Test lanes

The same suite runs in several configurations:

```bash
bash scripts/run_tests.sh                          # default JIT lane
bash scripts/run_tests.sh --stdlib=libc++          # the libc++ flavored lane
bash scripts/run_tests.sh --exe                    # native-executable lane (-o per test)
bash scripts/run_tests.sh --obj                    # native-object lane (-c, link, run)
MADC_BIN=bin/madc-release bash scripts/run_tests.sh  # the packed release suite
TESTS="testfoo* testbar*" bash scripts/run_tests.sh  # targeted subset (env var)
```

The libc++ lane has full behavior-parity with the default flavor; its
measurement history and regression gate is
`docs/parity/libcxx-failset.txt`.

`make -C src fulltest` also runs the repository gates (one-delimiter-tracker,
rule trailers, ownership ratchets, forest oracles, the warning ratchet, …) —
a change is not done until fulltest is green.

## Unit Tests

Unit tests use [doctest](https://github.com/doctest/doctest) (single-header,
`include/doctest.h`). Test files live in `tests/unit/` and cover the compiler
internals directly: the CIR tree and freeze/thaw, class layout and patterns,
the value pool, the config reader, the libmadc embedding API, and the
storage drivers.

Build and run:
```bash
make -C src test
```

### Writing a New Unit Test

Create `tests/unit/test_<name>.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "madc.h"

TEST_SUITE("My component") {
    TEST_CASE("something works") {
        CHECK(1 + 1 == 2);
    }
}
```

The Makefile's `test` target picks up all `tests/unit/*.cpp` files.

**Notes:**
- `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` exactly once per binary.
- Define `madc_verbose` and the `DBG` macro before including project headers.
- Do not define the `dd*` global instances — they come from the linked
  `parser.o` (via `TESTOBJ`).
- Use `CHECK((flags & vfFOO) != 0)` — doctest forbids bare bitwise AND inside
  `CHECK`.
- Use `CHECK_THROWS(...)` for expected exceptions.

## Verbose Debugging

When a test fails unexpectedly, run with `-v` to see trace output:

```bash
bin/madc -v tests/failing_test.mad 2>&1 | head -50
```

This shows tokenizer output, parse steps, and lowering decisions. For a
suspected codegen or runtime bug, reduce the failing case and compare against
`gcc`/`clang` first — see `.claude/rules/gcc-methodology.md` and
`.claude/rules/clang-methodology.md`.
