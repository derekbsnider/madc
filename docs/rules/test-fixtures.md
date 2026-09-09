# Test Fixtures — Reasoning

See `.claude/rules/test-fixtures.md` for the rules themselves.

## The principle

Test setup and expected output are data that *belong to the test*. Hiding
them in the runner script is poor separation of concerns: the runner has
to know what each test needs, and a reader of the `.mad` file can't see
how the test is actually exercised without digging through shell code.

The fix is to make the runner a mechanical iterator and let each test
carry its own fixtures.

## Why co-location matters

**High cohesion.** A `foo.mad` / `foo.input` / `foo.expect` trio is one
conceptual unit. Reading the test tells you: the code under test, what
the test feeds it, and what it expects back.

**Low coupling.** The runner depends only on the filename convention, not
on any individual test. Adding a new stdin-driven test is a filesystem
operation — drop a `.input` next to the `.mad`. No runner edits, no
merge conflicts on `run_tests.sh`.

**Self-documenting.** Anyone can ls `tests/` and see which tests consume
stdin, which take args, and which assert on specific output, just from
the presence of the fixture files.

## Why shell redirection beats pipe-in

For stdin input, `bin/madc foo.mad < foo.input` is preferred over
`echo "…" | bin/madc foo.mad`:

- The input is in a dedicated fixture file, easy to edit and extend to
  multi-line input.
- A reader who grep's `foo` finds the input file directly by name.
- The runner's shell expression stays readable — it's just "run program
  with optional stdin fixture."

`echo | prog` buries the input in the runner and has to be re-built
whenever it changes. With a fixture file, the runner doesn't care.

## Why `.expect_err` exists

Loud-diagnostic work (the no-ctor-match error, the `--std=` hard-error
gates) needs regression tests asserting that a program *fails* to compile
with a specific message. The plain `.expect` contract requires exit 0, so
it cannot express this. `.expect_err` inverts the contract for that one
test: nonzero exit (a timeout still fails) and the listed lines must
appear on stderr — the diagnostics are the expected output. The EXE pass
skips these tests because the source does not compile by design.

## Why `.expect_quiet` exists

`.expect` only requires the listed lines to *appear*, so a test whose
output is correct but whose compile leaks warnings or recovered-error
noise to stderr still passes — the noise is invisible to the suite.
Diagnostic-hygiene work (task #55: speculative template-instantiation
failures during overload scoring must be SFINAE-silent, matching g++)
needs the inverse assertion: this compile+run produces *no* stderr at
all. `.expect_quiet` (presence-only; content ignored) makes the runner
capture stderr separately and fail the test if any byte lands there,
reported as `NOISY(stderr):`. Use it on tests that compile real system
headers, where a reintroduced diagnostic leak would otherwise regress
silently.

## How to add a new capability

If the runner needs a new knob (say, compiler flags or environment variables), resist the
urge to add a per-test `case` branch. Instead, pick another filename
convention — e.g. `tests/foo.flags` with whitespace-split CLI flags or
`tests/foo.env` with `KEY=value` lines — and extend
the runner's per-test block to discover it. The runner remains free of
test-specific knowledge.

## Why `exe_skip` exists (2026-07-20)

The native-artifact lanes compile every JIT-green test to a native
artifact and byte-compare the run: `--exe` links a standalone executable,
`--obj` (AOT R4b) emits a relocatable `.o` and executes it through the
in-process loader (`madc foo.o`). A few tests exercise machinery that only
exists inside a live, freshly-compiled madc program — `testfreezerun`
re-executes the freeze/thaw container path, `testmadcevalexprctx` drives
in-process libmadc host callback eval contexts. The native lanes have no
analogue for these, so failing them would be noise, and hard-coding their
names into the runner would violate the no-per-test-logic rule. One
fixture covers all native lanes — the skip reason ("structurally
JIT-only") is a property of the test, not of any one artifact format. The
fixture file's content is a one-line justification, so every skip is
self-documenting — an empty or vague `exe_skip` in review is a red flag
that someone is hiding a real failure.

`obj_skip` is the narrower sibling: it exempts a test from the `--obj`
pass only. The four `--project` tests need it because a multi-TU program
has no single-`.o` form — `-c` on a project correctly emits one object
per TU, and running any one of them is not the program. The `--exe` lane
still covers those tests (a whole-project executable IS a single
artifact), which is exactly why they must not use `exe_skip`. When
multi-object loading lands (the declared future rung), these four
fixtures are the removal checklist.

## Why `<domain>_skip` and `<domain>_expect` exist (2026-08-13)

The Windows lane runs the same suite against a binary targeting a
different execution DOMAIN (win64; under wine that run is two domains
at once, `MADC_SKIP_EXT="win64 wine64"`). Two per-domain facts are
properties of individual tests, not of the runner:

- **Structurally out of domain** — the test asserts something the
  domain's own oracle compiler rejects or cannot express (for example,
  POSIX sockets, which mingw-gcc rejects too).
  `<domain>_skip` carries the one-line reason; the summary line labels
  the run a DOMAIN RUN so it can't be quoted as the default baseline.
- **Correct output differs** — the test is meaningful on the domain but
  its right answer is different there (`sizeof(long)`-derived values on
  LLP64, `%lu` reading 4 bytes). `<domain>_expect` replaces `.expect`
  for that run; its content MUST come from the domain's oracle compiler
  (mingw-gcc/g++ for win64), never be reverse-engineered from madc's
  own output — otherwise the fixture just ratifies whatever madc does.

The first listed domain with a fixture wins, so layered runs (wine over
win64) resolve deterministically.

### Twins drift — the `.domain` sidecar and `check-expect-twins.sh` (2026-09-08)

A twin REPLACES the base, so every line of the base that is not a domain
difference is a COPY in the twin — and a copy drifts. It happened on the
colour-unification slice: the fix to `testmadcide.expect`'s `spans:` line
never reached `testmadcide.win64_expect`; the win64 twin kept the old
answer and the wine lane went red two hours later, at the end of the
merge-wave battery, on content every other lane had passed. The gate
`scripts/check-expect-twins.sh` (fulltest) closes it for LABELLED lines
(`label: rest`, the clause labels a headless IDE test prints): a base
label must appear in the twin with the identical line, unless the twin's
`.domain` sidecar names it — the labels whose answer differs on that
domain (`build-out` under wine, `platform` on darwin/win64), or which the
domain does not print (the pointer clauses on win64). Naming the
differences makes them reviewable; the silent kind was the bug. Unlabelled
twins (a C program's plain output) are not gated — their whole point is a
different answer — and the domain-oracle rule above still governs what a
twin may say.

## The `.helper` marker — why it exists

`tests/<name>.helper` marks a `.mad` file that is a compilation unit of
another test (an `#include` module, a `--project` TU source) rather than
a standalone test. Before the convention, `include_helper.mad` was
excluded by NAME in six different scripts (`run_tests.sh`,
`cir_fidelity.sh`, `class_parse_burndown.sh`, `forest_soak.sh`,
`tsubst_burndown.sh`, `warn_census.sh`) — six hand-rolled copies of one
rule, exactly the drift pattern the first bullet of the rules file
forbids, and every new helper file would have needed a seventh, eighth,
… edit. The marker moves the fact onto the test it describes (one
fixture file, co-located, contents name the owner) and every enumerator
consults it the same way it consults the other fixtures.

## The `.env` fixture (2026-09-07)

madc arms no resource guard by default (owner ruling 2026-09-07); the suites'
caps are the RUNNER's to ask for, so `scripts/run_tests.sh` exports
`MADC_MEM_LIMIT=auto` once and every test process inherits it. A test whose
point is the unguarded behaviour (`tests/testguardsoff.mad` allocates and
touches 6 GB of address space) needs the opposite for itself only — a
per-test environment. `tests/<base>.env` holds whitespace-split `NAME=value`
pairs that the runner hands to `env(1)` in front of every invocation of that
test (JIT, exe and obj alike), exactly as `.argv` and `.input` shape the
other two ends of the process. No `MADC_TEST_ENV_<name>` switch, no per-test
branch: the fixture file is the convention.
