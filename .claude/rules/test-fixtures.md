# Test Fixture Rules

- Never hard-code individual test names in `scripts/run_tests.sh` (or any
  similar runner). No `case "$base" in testfoo.mad) …` branches.
- Per-test setup lives in sibling fixture files, discovered by the runner
  via filename convention:
  - `tests/foo.flags` — whitespace-split compiler flags prepended before the source path
  - `tests/foo.input` — redirected to stdin
  - `tests/foo.argv` — whitespace-split, appended as argv
  - `tests/foo.expect` — each non-empty line must appear in the output
  - `tests/foo.expect_err` — compile-error test: must exit nonzero (not a
    timeout) and stderr must contain each non-empty line; EXE pass skips it
  - `tests/foo.expect_quiet` — JIT run must produce EMPTY stderr (content
    of the fixture file is ignored; presence enables the check)
  - `tests/foo.mir_skip` — skip this test when running with `--backend=mir`
  - `tests/foo.exe_skip` — skip this test in the native-artifact passes
    (`--exe` and `--obj`; content = one line saying why the test is
    structurally JIT-only)
  - `tests/foo.obj_skip` — skip this test in the `--obj` pass only
    (content = one line saying why it is outside the single-object
    domain, e.g. a multi-TU `--project` program; the `--exe` pass still
    covers it)
  - `tests/foo.timeout` — per-test wall-clock cap in seconds (default 5); raise it for a legitimately slow test (e.g. a real-libstdc++-header compile, no PCH yet)
  - `tests/foo.<domain>_skip` — skip when `MADC_SKIP_EXT` (a whitespace-split
    domain list, e.g. `"win64 wine64"`) includes `<domain>`; content = one
    line saying why the test is structurally out of that domain
  - `tests/foo.<domain>_expect` — replaces `.expect` when `MADC_SKIP_EXT`
    includes `<domain>` (first listed domain with a fixture wins); for tests
    whose CORRECT output differs on the domain — content comes from that
    target's oracle compiler (e.g. mingw-gcc for win64)
  - `tests/foo.<domain>_obj_skip` — skip the `--obj` pass only, only when
    `MADC_SKIP_EXT` includes `<domain>`; for tests whose `.o` lane is
    structurally out of that domain's scope while the JIT (and other
    domains' `.o` lanes) still cover them; content = one line saying why
- When a test needs stdin, use a `.input` fixture file with shell
  redirection (`prog < foo.input`). Never use `echo ... | prog` inline in
  the runner.
- When the runner needs a new capability, add it as another generic
  filename convention, not a per-test hook.

See `docs/rules/test-fixtures.md` for the reasoning and design notes.
