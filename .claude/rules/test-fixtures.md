# Test Fixture Rules

- Never hard-code individual test names in `scripts/run_tests.sh` (or any
  similar runner). No `case "$base" in testfoo.mad) …` branches.
- Per-test setup lives in sibling fixture files, discovered by the runner
  via filename convention:
  - `tests/foo.flags` — whitespace-split compiler flags prepended before the source path
  - `tests/foo.input` — redirected to stdin
  - `tests/foo.argv` — whitespace-split, appended as argv
  - `tests/foo.expect` — each non-empty line must appear in the output
- When a test needs stdin, use a `.input` fixture file with shell
  redirection (`prog < foo.input`). Never use `echo ... | prog` inline in
  the runner.
- When the runner needs a new capability, add it as another generic
  filename convention, not a per-test hook.

See `docs/rules/test-fixtures.md` for the reasoning and design notes.
