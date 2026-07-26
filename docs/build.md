# Building madc

## Requirements

- `clang++` or `g++` with C++11 support
- The **madc MIR fork** (libmir + c2mir) at `/workspace/mir` —
  [github.com/derekbsnider/mir](https://github.com/derekbsnider/mir), branch
  `develop`. This is **not** upstream MIR: the fork carries native C99
  `_Complex`, `__attribute__((cleanup))`, ≤16-byte SIMD/vector support, and
  ABI/codegen fixes the CIR backend depends on. The exact commit is pinned by
  the repo-root `MIR_COMMIT` file and the fork release it corresponds to by
  `MIR_VERSION`; see `.claude/rules/build.md` for the pin and release
  discipline.
- `libzstd-dev` (the packed forest's codec). `./configure --without-zstd`
  accepts a zlib fallback, which produces much larger packed binaries and
  slower binds — not recommended.
- `autoconf`, if you change `configure.ac` (`configure` itself is generated and
  not tracked).

The backend is `parser → cir_node (MC11-IR) → c2mir → MIR`. asmjit — the
original x86-64 JIT — was removed; there is no second codegen path.

## Configure

```bash
./configure                     # defaults
```

Optional axes:

| Option | Default | Effect |
|--------|---------|--------|
| `--enable-madcdat` | yes | build the madcdat storage/federation support. `--enable-madcdat=no` shrinks the rebuild and unit-test footprint for core compiler work. |
| `--without-zstd` | (zstd on) | accept the zlib fallback for the packed forest. |
| `--with-forest=embedded\|sidecar\|none` | embedded | what the PRODUCT build ships: the container packed into the binary's own image, a `<binary>.forest` sidecar beside it, or nothing (live parse). |
| `--enable-shared` | no | link the CLI against the shared libmadc (thin CLI) instead of statically. |
| `--enable-config-file` | yes | madc looks for a `madc.ini`. `--disable-config-file` removes that lookup — such a build never searches for nor opens one. |

`./configure` regenerates `config.mk`; the Makefile has matching defaults, so an
unconfigured tree still builds.

## Build

```bash
make -C src           # develop mode: -O0 -> bin/madc
make -C src debug     # -O0 -ggdb, unstripped -> bin/madc-debug
make -C src release   # -O2, stripped, forest-packed -> bin/madc-release
make -C src clean     # remove all object trees
```

Modes never share objects or output binaries: each compiles into its own
`obj/<mode>/` tree (plus `obj/<mode>/pic/` for the shared library) and links its
own `bin/` name, so a dev build and a packed release build coexist. `-MMD`
tracks headers, not flags, which is why mixing modes in one tree is not allowed.

`BUILD_TESTS=0` skips the unit-test binaries for a faster library-and-CLI
iteration build.

## Output

- CLI: `bin/madc` (or `bin/madc-debug` / `bin/madc-release`)
- Libraries: `lib/libmadc.a`, `lib/libmadc.so`, and `lib/libmadcdat.a` when
  madcdat is enabled
- Objects: `obj/<mode>/` and `obj/<mode>/pic/`
- Unit-test binaries: `bin/test_*`

## Tests

```bash
make -C src test      # unit tests (doctest) — tests/unit/*.cpp
make -C src fulltest  # unit + integration tests + every gate. THE gate for "done"
bash scripts/run_tests.sh --exe          # native-executable lane
MADC_BIN=bin/madc-release bash scripts/run_tests.sh   # the packed suite (arbiter)
```

Integration tests are `tests/*.mad` with fixtures discovered by filename
convention (`.flags`, `.input`, `.argv`, `.expect`, …) — see
`.claude/rules/test-fixtures.md`. Every test invocation runs under both a
wall-clock `timeout` and a `ulimit -t` CPU cap, so a hang fails the suite
instead of pegging the host.

Current pass counts live in `docs/test-status.md` and `README.md`.

## Install

```bash
sudo make -C src install            # bin/madc, the shared library, the man page
sudo make -C src install-libmadc    # the embedding library + its public headers
```

`PREFIX` (default `/usr/local`) and `DESTDIR` are both honoured.

## Running a program

```bash
bin/madc tests/testint.mad          # JIT-run a script
bin/madc -v tests/testint.mad       # with tokenizer/parser/backend traces
bin/madc -o prog prog.c             # emit a native executable (no external toolchain)
```

A `.mad` file carrying a shebang can be made executable and run directly — the
tests use a tree-relative `#!../bin/madc`; an installed madc takes
`#!/usr/bin/env madc`.
