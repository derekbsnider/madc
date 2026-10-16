# Command-line options — one engine, madc's CLI on it, dialect faces

Status: DESIGN (owner-approved direction, 2026-10-16). Spec for review.

## 1. Why

madc's driver reads its command line with a ladder of 82 `strcmp` /
`strncmp` arms in `main()` (`src/madc.cpp`). Each arm decides on its own what
an option looks like, which options take values, and when the program's
own argv begins. The SQLite stretch of the 0.106.0 arc showed the cost:
`-E` ignored `-o`, `cc x.c -o x -lfoo` ran the program, and `-Wall` was read
as a file. Each was fixed in its own arm (`31283e348`, `0942a4f1f`,
`6cb2f8c67`); the shape stays ad hoc.

Dialect programs have nothing at all: each walks `madc::sys.argv` with its
own `strcmp` chain (`examples/vised/vised.mad`), and madcide parses its own
argv separately. The owner law in `.claude/rules/dialect-faces.md` says a
capability madc builds for itself is reachable from the dialect, so a host
calls madc's code and never keeps a copy.

## 2. Owner decisions (2026-10-16)

- **One syntax, every OS: GNU dashes.** No `/opt` switches on Windows. The
  industry has moved to dashes there too, and gcc / clang / mingw behave the
  same on all three OSes.
- **Forward-slash paths everywhere.** Every Win32 file API accepts `/`
  (drive and UNC paths included). madc accepts either separator on input,
  stores and prints `/`, and converts to `\` only at the two places that
  need it: a command line handed to `cmd.exe` and the `\\?\` long-path
  prefix. Through the existing path owners (`canonical_path_for_compare`,
  `host_path_absolute`, `host_path_within`), never a new helper.
- **Easy and familiar.** Python's argparse is the familiarity target.
  boost::program_options, picocli, Rust's clap and C++26 static reflection
  are the models.
- **Sequencing:** the engine and madc's own CLI first, then
  `python::argparse`, then the typed `madc::cli` struct face (it needs
  member annotations in the compiler).
- **`madc::sys` is part of it** (owner reminder): `sys.argv` is what the
  faces read, and madc's own parsed options are exposed beside it.

## 3. What the models agree on (recon, 2026)

- Declare once; the declaration drives parsing, typed values, defaults,
  validation, usage / help, errors, and (picocli, clap) completion scripts
  and man pages.
- One schema, several sources (boost's `options_description`): command
  line, environment, config file, with a stated precedence. clig.dev:
  flags > environment > project config > user config > defaults.
- Typed declarations are the current default: clap derive (a struct),
  picocli (annotated fields), Swift ArgumentParser (property wrappers),
  Typer (type hints), C++26 reflection (annotated members). A misspelled
  option is a compile error. argparse's string-keyed namespace is the
  familiar exception: a typo reads as nothing.
- Shared conventions: `-abc` bundling, `-ofile` / `-o file`, `--long=v` /
  `--long v`, `--` ends options, `@file` response files, `--no-x`
  negation, "did you mean `--output`?", exit status 2 for a usage error,
  NO prefix abbreviation of long options by default (adding an option
  later silently changes what an abbreviation meant).

## 4. The engine (C++ API first — `cpp-first-api.md`)

One owner, in libmadc: `include/libmadc/cli_options.h` (C++), implementation
`src/cli_options.cpp`. Every other surface is a face over it.

### 4.1 The schema

An option is a row:

| field | meaning |
|---|---|
| `id` | an integer code: the caller's enum value (C++), a struct member (typed face), a generated code (argparse face). Results are read by `id`, never by spelling. |
| `spellings` | `-o`, `--output`, and for a table that allows it, a single-dash long word (`-std=`, `-shared`) |
| `kind` | flag · count (`-vvv`) · value · list (repeatable) · joined-only (`-Wl,...`) |
| `type` | string · int · float · bool · enum (a closed set of words) · path |
| `metavar`, `help` | for usage / help |
| `default` | when no source sets it |
| `env` | an environment variable that supplies it (boost / clap `env =`) |
| `config_key` | its key in `madc.ini` (`src/madc_config_file.cpp` / `madc_config.cpp` read it) |
| `required`, `choices`, `group` | validation; a group is mutually exclusive or "requires" |
| `negatable` | accepts `--no-<long>` |
| `hidden` | parsed, not shown in help |

Positionals are rows too (`nargs`: one, optional, many, remainder).
Subcommands are a table per command (`madc session list ...` later; argparse
`add_subparsers`, picocli `@Command(subcommands=...)`).

### 4.2 Syntax rules (one reader)

- GNU: `-abc` bundling for flag rows, `-ofile` and `-o file`, `--long=v`
  and `--long v`, `--`.
- Single-dash long words for tables that declare them (madc's own CLI:
  `-std=c17`, `-shared`, `-fPIC`, `-Wall`), matched as whole words before
  bundling is tried.
- `@file` response files, expanded in place, recursively, with gcc's
  quoting rules (gcc, MSVC, argparse `fromfile_prefix_chars`, picocli all
  have them; it is also how a long Windows command line fits its limit).
- Permutation: options may follow operands (gcc), unless the table says
  "stop at the first operand" — madc's script lane, where everything after
  a `.mad` script is the script's argv (owner rule of `0942a4f1f`).
- No abbreviation of long options.

### 4.3 Sources and precedence

command line > environment (`env`) > `madc.ini` (`config_key`) > `default`.
The result records which source set each value (for `--help`, diagnostics
and madcide's settings view).

### 4.4 Outputs

- A result: typed values by `id`, the operands, the remainder (a script's
  argv), and the source of each value.
- Errors: unknown option with the nearest spelling ("did you mean"),
  missing value, bad type, choice not in the set, group conflict. Exit
  status 2 for a usage error, as argparse and clig.dev do.
- Generated usage and help, wrapped to the terminal width, grouped by the
  table's sections.
- Completion scripts (bash, zsh, fish, PowerShell) generated from the table.
- A JSON description of the table (`madc --help=json`), the shape
  `--capabilities=json` already uses, for tools and for madcide.

### 4.5 Portability

- Windows: madc.exe and every executable madc writes carry an application
  manifest with `<activeCodePage>UTF-8</activeCodePage>` (Windows 10 1903
  and later), so argv, the environment and every ANSI API are UTF-8 with no
  `wmain` fork. Today madc reads argv in the ANSI code page: a path with a
  non-ASCII character is lossy (found 2026-10-16; no `wmain` /
  `GetCommandLineW` anywhere). The PE writer already embeds resources (the
  icon), so the manifest rides the same path.
- Path-typed values are normalized through the path owners above.
- Behaviour is identical on Linux, macOS and Windows; the tests run on all
  three lanes.

### 4.6 Thread-safety contract (`thread-safety.md`)

A table is immutable once built and may be shared across threads. A parse
is reentrant: it reads the table and its own inputs and returns a new
result. A result is a value owned by its caller. No global state.

## 5. madc's own CLI on the engine

- One `enum class cli_option` and one table replace the 82 arms; `main()`
  becomes a `switch` over the parsed result (`enum-over-strings.md`).
- Behaviour-preserving first: every option keeps its spelling and meaning,
  including the rules the ladder grew one at a time — the artifact lane's
  permutation, a C/C++ source with a stage option after it is a compiler
  line, a `.mad` script keeps its argv, `-Werror` refused, warning
  selections accepted. The suite is the oracle for that slice.
- madc's help text becomes the generated one; `--help=json` describes it.
- Gate: `scripts/check-one-option-parser.sh` fails on a `strcmp(argv[` or
  `strncmp(argv[` in `src/` outside the engine, with a negative control.
- This consolidation is the 0.106.0 release's `Consolidated:` family
  candidate together with `bare_function_name_call_or_address` (owner law
  `4c63204d5`), if it lands in this arc.

## 6. `madc::sys`

- `sys.argv` stays the input every face reads by default (argparse reads
  `sys.argv[1:]`); on Windows it is UTF-8 by the manifest above.
- New, appended at the end of `SysInfo` (the layout contract with
  `include/libmadc/sysinfo.h` is append-only), named as Python names them
  (owner, 2026-10-16):
  - `sys.flags` — the running madc's own parsed options, a read-only `var`
    object keyed by each option's table name. Python's `sys.flags` is a
    fixed set of interpreter settings; madc's carries every option in the
    table, the same role.
  - `sys.orig_argv` — the whole original command line, madc's own options
    included (Python 3.10+); `sys.argv` stays the program's.
- A face returning madc's option TABLE as data (the `--help=json` shape).
  madcide reads `sys.flags` and the table instead of keeping its own copy;
  registered in `scripts/dialect_faces.tsv` (`dialect-faces.md`). This is
  the arc's "madcide gate for hand-copied madc tables" for the option table.

## 7. Dialect faces

### 7.1 `python::argparse` (familiarity)

argparse's API over the engine: `ArgumentParser(prog, description)`,
`add_argument(names..., action, type, default, help, choices, required,
nargs, metavar, dest)`, `add_mutually_exclusive_group`, `add_subparsers`,
`parse_args()` (default `sys.argv[1:]`), `parse_known_args`, `print_help`,
`error`. The namespace is a `var` object keyed by `dest`. Lean primary forms
(`var`, `const char *`), no std::string (`dialect-lean.md`). Divergences
from CPython are named in its doc page (no prefix abbreviation, because the
engine refuses it).

### 7.2 `madc::cli` (typed, native)

A struct whose members are the options, each annotated with its spellings
and help; `madc::cli::parse<options>()` fills it from `sys.argv`. Results
are typed members: a misspelled option is a compile error, which satisfies
the enums-over-strings law without the user writing an enum. This needs
member annotations the compiler can read at compile time — the C++26
direction (static reflection + annotations) — so it is the last slice and
advances C++ parity on the way. Until it lands, the literal-spec form of
the engine (an object literal of rows, read by an enum) is available to
dialect code.

## 8. Slices

1. Engine + unit tests (doctest): every syntax rule, each source, each
   error, the response-file quoting; oracles: gcc's own option handling
   for its rules, CPython argparse for the argparse face's.
2. madc's CLI on the engine, behaviour-preserving; the gate; generated
   help; `--help=json`. Tier 2 + the full batch lane (every test's
   `.flags` exercises it).
3. Windows UTF-8: the manifest in madc.exe and in produced executables;
   a test with a non-ASCII argument on the genuine-Windows lane.
4. `madc::sys.flags`, `sys.orig_argv` and the table face; madcide reads them (its copy
   deleted — a move is done when the host's copy is gone).
5. `python::argparse`.
6. Member annotations + the typed `madc::cli` face.

## 9. Settled (2026-10-16)

- **madc stays gcc-shaped (owner).** No subcommands at the top level: `CC=madc`
  keeps working unchanged. A command-like mode madc ever needs is a new
  OPTION (as `-i`, `--lsp`, `--project` already are), not a subcommand. The
  engine's subcommand tables serve dialect programs, not madc's own CLI.
- Completion scripts (bash, zsh, fish, PowerShell) ship with slice 2: they
  are generated from the table, so they cost little once it exists. Man
  pages come later.
- `env` / `config_key` rows: at first only the options `madc.ini` and the
  existing `MADC_*` environment variables already carry; more are added one
  by one, each with its test.
