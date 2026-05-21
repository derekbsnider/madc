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

## How to add a new capability

If the runner needs a new knob (say, compiler flags or environment variables), resist the
urge to add a per-test `case` branch. Instead, pick another filename
convention — e.g. `tests/foo.flags` with whitespace-split CLI flags or
`tests/foo.env` with `KEY=value` lines — and extend
the runner's per-test block to discover it. The runner remains free of
test-specific knowledge.
