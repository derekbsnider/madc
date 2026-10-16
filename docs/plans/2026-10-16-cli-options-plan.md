# Command-line options, slices 1–2 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One option reader in libmadc (`madc::cli`) that reads a command line against a declared table and derives help, a JSON description and shell completions from the same rows. Then madc's own 83-arm `strcmp` ladder in `main()` is replaced by one table and one `switch`, with its behaviour preserved.

**Architecture:** A program's options are a constant table of aggregate rows: spellings with their value forms, metavar, help, type, choices, default, env, config key and flags. `madc::cli::parse()` is the one reader. It implements gcc/GNU syntax (exact word, `NAME=VALUE`, longest glued spelling, `--no-NAME`, optional bundling), `--`, `@file` response files, permutation under an operand rule, typed validation, groups, and the environment / config / default sources. Writers turn the same table into help, `--help=json` and bash/zsh/fish/PowerShell completion scripts. madc's own table lives in `src/madc_cli.cpp`, beside its operand rule (gcc's permutation on an artifact lane; a `.mad` script keeps its argv). `main()` applies the parsed occurrences in order through a `switch` on `enum class cli_option`.

**Tech Stack:** C++11, doctest (`include/doctest.h`), nlohmann::json (`include/json.hpp`), bash gates, madc CLI integration tests (`tests/testcli_*.mad`).

**Spec:** `docs/plans/2026-10-16-cli-options-design.md` — sections 4, 5 and 9; slices 1 and 2 of section 8. Slices 3–6 (Windows UTF-8 manifest, `madc::sys.flags` / `sys.orig_argv` and the table face, `python::argparse`, the typed `madc::cli` face) get their own plans.

## Global Constraints

- **Build host:** the NAS never builds or runs madc. Sync and build on the container with `bash scripts/remote_build.sh sync build`, then restore the owner's file: `ssh -p 2299 dev@localhost 'cp -p /workspace/madc/tmp/runprobe/hello.cpp /workspace/madc/hello.cpp'`. One build at a time.
- **A unit binary:** `ssh -p 2299 dev@localhost 'cd /workspace/madc && make -C src ../bin/test_cli_options && bin/test_cli_options'`. Every `tests/unit/test_X.cpp` builds `bin/test_X` automatically, linked against every object except `madc.o`.
- **Per commit that touches `src/` or `include/`:**
  - Tier 1 + Tier 2 as one command: `TESTS='<globs>' bash scripts/remote_build.sh sync build fix`.
  - The static gate loop on the container: `ssh -p 2299 dev@localhost 'cd /workspace/madc && fail=0; for g in scripts/check-*.sh; do ( ulimit -t 300; timeout 300 bash "$g" ) >/dev/null 2>&1 || { echo "RED $g"; fail=1; }; done; echo "gates fail=$fail"'`.
  - The unit binaries the task names.
- **Commits:** a commit touching `src/` or `include/` is landed by the `commit-writer` agent. Stage the change and give it only pointers: the staged diff, the unit test or reducer, and the spec section. It composes the four rule trailers and any CHANGELOG entry, then commits. Verify with `git log -1` and `bash scripts/check-rule-trailers.sh`. Commits that touch only docs, scripts or tests may be written directly with `git commit -F`.
  - Attribution lines end every commit message:
    `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
    `Claude-Session: https://claude.ai/code/session_01TyHLoR3nwVP6PmWcHkmeic`
- **Shell hygiene:** one simple command per Bash call. No `&&` chains and no shell variable substitution in local commands; ssh-quoted remote scripts are the exception. Scratch files go in `tmp/`.
- **Code:**
  - C++11 (`-std=c++11`): no designated initializers, no `std::optional`, `string_view` or structured bindings. A multi-statement lambda states its return type (`-> bool`).
  - Tabs for indentation. Header guards are `#ifndef __NAME_H` / `#define __NAME_H 1`.
- **Strings:**
  - A table holds names and help as `const char *` literals: constant data, never constructed at run time.
  - A name held at run time is `madc::dis::istring` (owner law, 2026-10-08). Built text (values, words, messages) is `std::string`.
  - The new core files join `scripts/check-istring-fields.sh`'s CORE list with a recorded count.
- **Syntax:**
  - GNU dashes on every OS; no `/opt` switches.
  - No prefix abbreviation of long options.
  - madc stays gcc-shaped: no subcommands. A mode madc needs is a new option.
- **Exit status:** the reader reports and the caller chooses the status. madc exits 1 on a usage error, as gcc does. Exit 2 belongs to the dialect faces (argparse), which come in a later slice.
- **Pinned messages stay word for word** (tests and gates read them):
  - `Unknown -fdiagnostics-color= value: X (auto, always, never)`
  - `Unknown capabilities format: WORD (json)`
  - `Unknown --std target: X`
  - `Unknown --emit target: X (TARGETS)`
  - `Unknown -stdlib flavor: X (this madc was built with: LIST)`
  - `madc: WORD: not supported (madc's warnings are never errors)`
  - `madc: WORD: linker option 'X' is not supported (-rpath is)`
  - `madc: --config= needs a file path`
  - `madc: --config= and --no-config may not be used together`
- **Thread-safety contract** (spec 4.6, stated in the header): a table is immutable and may be shared across threads; `parse()` is reentrant; a result is a value its caller owns; no global state.
- **Start-up cost:** the table is constant-initialized data, and parsing scans it the way the old ladder scanned its arms. The seam battery's parse-cost gate checks this against `scripts/parse_cost/baseline.tsv`: 0.5% tolerance; the smallest workload, `rung_stddef.forest`, is 12.7M Ir.
- **Deliberate behaviour changes** (each has a test in Task 7):
  1. An unrecognized option is an error: `madc: error: unrecognized command-line option '-x'`, with `; did you mean '...'?` when one is close. Before, it was opened as the source file.
  2. A missing value is an error: `madc: error: missing argument to '-o'`. Before, a trailing `-o` was opened as a file.
  3. `--` ends madc's options.
  4. `@file` response files expand while madc's options are being read (gcc), never inside a script's argv.
  5. `-std=` (gcc's spelling) is accepted beside `--std=`.
  6. `--emit-object`, `--emit-executable` and `--dump-source` count as stage options in the compiler-line rule. `madc x.c --emit-object x.o` builds; before, it ran `x.c`.
  7. `-Wa,` and `-Wp,` are refused by name.
  8. New options: `--help=json` and `--completion=SHELL`.
  9. The help is generated from the table. It lists what the old text omitted: the `modules` and `theme` madc.ini keys, `-dumpmachine`, the `-fPIC` family and `-feval-shims`.
- **Scope cut (for the owner's review):**
  - madc's config step (madc.ini and `MADC_*`) is not moved onto the reader's sources in this plan. Its precedence has arms the uniform rule cannot express: the source's extension sits between the CLI and madc.ini for `std`, the forest has its discovery arms, and `include` is additive. madc's rows therefore carry no `env` / `config_key` yet. The reader's sources serve the dialect faces.
  - Positional rows (`nargs`) and subcommand tables (spec 4.1) come with the argparse face (slice 5): `add_argument` positionals and `add_subparsers` are their first users. madc's own CLI has neither (spec 9: madc stays gcc-shaped). Until then madc's operands are documentation rows (`rf_doc`).
  - Exit status 2 for a dialect usage error belongs to the faces. The reader only reports.

## Review Focus

1. A script's own argv that looks like options or response files stays the script's: `madc tool.mad -o out @args -- -v` passes every word after `tool.mad` through untouched. *(Task 6 unit test; Task 7 integration line `script argv`.)*
2. A response file that names itself, or a cycle, ends with an error instead of hanging or exhausting memory; a directory is refused by name. *(Task 2.)*
3. An option value that starts with a dash is a value, as in gcc: `-o -x`, `-I -dir`. *(Task 1.)*
4. Empty and degenerate forms give clean errors and are never opened as a file: `-o` or `-l` at the end of the line, `-fdiagnostics-color=` with nothing after the `=`. *(Task 1 for `-o` and `-l`; Task 7 integration line `colour empty` for the `=` case, which must keep its pinned message.)*
5. madc's own aliases count as stage options in the compiler-line rule: `x.c --emit-object x.o` builds and does not run. *(Task 6 unit test; Task 7 integration line `emit-object`.)*

## File structure

| File | Responsibility |
|---|---|
| `include/libmadc/cli_options.h` (new) | The reader's public C++ API: schema (rows, sections, groups, table), result, sources, `parse`, `match_word`, `find_row`, and the writers |
| `src/cli_options.cpp` (new) | The reader, its validation and sources, and the help / JSON / completion writers |
| `tests/unit/test_cli_options.cpp` (new) | doctest for the reader and writers, over test tables |
| `include/madc_cli.h` (new) | madc's own `enum class cli_option`, its tag bits, `madc_cli_table()` |
| `src/madc_cli.cpp` (new) | madc's table: every row with its help text, the sections, the operand rule (lane + compiler line) |
| `tests/unit/test_madc_cli.cpp` (new) | doctest for madc's table invariants and its lane rule |
| `src/madc.cpp` (modify) | `main()`: the ladder becomes parse plus a `switch`; `print_usage`, `stage_option`, `compiler_line_source`, `warning_selection_option` deleted; help / JSON / completion output |
| `src/madc_capabilities.cpp` (modify) | `introspection` derived from rows tagged `mt_introspection` |
| `src/madc_posix_io.cpp`, `src/madc_posix_io.h` (modify) | `host_path_generic` beside the host path family |
| `src/madc_console.cpp`, `include/madc_console.h` (modify) | `terminal_columns()`, the one width rule; `src/lexer.cpp` reads it |
| `src/lexer.cpp`, `include/madc.h`, `src/madc_cir.cpp` (modify) | `Program::set_stdlib_flavor(name)` replaces `set_stdlib_flavor_option(arg)` |
| `src/Makefile` (modify) | `cli_options.o`, `madc_cli.o` in `CORE_OFILES` |
| `scripts/check-one-option-parser.sh` (new) | The gate: no hand-rolled argv option reads outside the reader |
| `scripts/check-istring-fields.sh`, `scripts/istring_keep.tsv` (modify) | The new core files join the ratchet |
| `tests/testcli_option_engine.{mad,expect,exe_skip,win64_skip}` (new) | madc's CLI on the reader, end to end |
| `docs/language/command-line.md` (new), `AGENTS.md` (modify) | User doc; the Rule #4 standing instance |

---

### Task 1: The reader — schema, gcc/GNU syntax, errors, suggestions

**Files:**
- Create: `include/libmadc/cli_options.h`
- Create: `src/cli_options.cpp`
- Create: `tests/unit/test_cli_options.cpp`
- Modify: `src/Makefile:135` (`CORE_OFILES`)
- Modify: `scripts/check-istring-fields.sh:41-46` (`CORE`), `scripts/istring_keep.tsv`

**Interfaces:**
- Consumes: nothing.
- Produces, used by Tasks 2–8:
  - The types `madc::cli::value_form`, `value_type`, `row_flag` (`rf_ends_options`, `rf_hidden`, `rf_negatable`, `rf_required`, `rf_doc`), `max_spellings`, `spelling`, `row`, `section`, `group_rule`, `max_group_ids`, `group`, `operand_rule`, `operand_policy`, `table`, `value_source`, `occurrence`, `error_kind`, `result`, `match`.
  - The functions `result parse(const table &, const std::vector<std::string> &)`, `match match_word(const table &, const std::string &)` and `const row *find_row(const table &, int id)`.
  - `row` field order, which every table initializer relies on: `id, name, spellings, metavar, section, help, usage, flags, tags, type, choices, default_value, env, config_key`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_cli_options.cpp`:

```cpp
// Unit tests for the command-line option reader (include/libmadc/cli_options.h;
// design docs/plans/2026-10-16-cli-options-design.md). Oracles: gcc's own
// option handling for the gcc-shaped rules (separate / joined / the longest
// spelling, "missing argument to", "unrecognized command-line option"), GNU
// getopt for bundling, `--` and permutation.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <initializer_list>
#include <string>
#include <vector>

#include "libmadc/cli_options.h"

namespace {

namespace cli = madc::cli;
using cli::value_form;

const value_form flag = value_form::none;

enum test_id
{
	t_verbose = 1, t_quiet, t_output, t_include, t_lib, t_opt, t_warn,
	t_werror, t_link, t_std, t_run, t_long, t_relayout, t_colour
};

const cli::section sections[] = {
	{ "General", NULL },
};

const cli::row rows[] = {
	{ t_verbose, "verbose", { { "-v", flag }, { "--verbose", flag } }, NULL, 0, "say more" },
	{ t_quiet, "quiet", { { "-q", flag } }, NULL, 0, "say less" },
	{ t_output, "output", { { "-o", value_form::separate } }, "<file>", 0, "write <file>" },
	{ t_include, "include", { { "-I", value_form::joined_or_separate } }, "<dir>", 0, "search <dir>" },
	{ t_lib, "lib", { { "-l", value_form::joined } }, "<name>", 0, "link <name>" },
	{ t_opt, "opt", { { "-O", value_form::prefix } }, "<level>", 0, "optimize" },
	{ t_warn, "warn", { { "-W", value_form::prefix } }, "<name>", 0, "warning selection" },
	{ t_werror, "werror", { { "-Werror", value_form::prefix } }, NULL, 0, "refused" },
	{ t_link, "link", { { "-Wl,", value_form::joined } }, "<opts>", 0, "linker options" },
	{ t_std, "std", { { "--std", value_form::equals } }, "<std>", 0, "language standard" },
	{ t_run, "run", { { "--run", value_form::optional_equals } }, "<file>", 0, "run" },
	{ t_long, "long", { { "--long", value_form::equals_or_separate } }, "<v>", 0, "a long value" },
	{ t_relayout, "relayout", { { "--relayout", value_form::equals } }, "<image>", 0,
	  "the rest are logs", NULL, cli::rf_ends_options },
	{ t_colour, "colour", { { "--colour", flag } }, NULL, 0, "colour", NULL, cli::rf_negatable },
};

cli::table make_table(cli::operand_rule rule)
{
	cli::table t = cli::table();
	t.program = "prog";
	t.usage = "[options] <file>";
	t.sections = sections;
	t.nsections = sizeof(sections) / sizeof(sections[0]);
	t.rows = rows;
	t.nrows = sizeof(rows) / sizeof(rows[0]);
	t.operands = rule;
	return t;
}

std::vector<std::string> words(std::initializer_list<const char *> w)
{
	return std::vector<std::string>(w.begin(), w.end());
}

// A caller rule: a .c operand keeps the options open (a compiler line); any
// other operand starts the remainder, unless an operand was taken already.
bool c_sources_continue(const cli::table &, const cli::result &so_far,
			const std::vector<std::string> &w, size_t at)
{
	if ( !so_far.operands.empty() )
		return true;
	const std::string &s = w[at];
	return s.size() > 2 && s.compare(s.size() - 2, 2, ".c") == 0;
}

} // namespace

TEST_CASE("options and operands permute, in order")
{
	const cli::table t = make_table(cli::operand_rule::permute);
	const cli::result r = cli::parse(t, words({ "-v", "a.c", "-o", "out", "-Iinc", "-I", "dir",
						    "-lm", "--std=c17", "b.c" }));
	REQUIRE(r.ok());
	REQUIRE(r.options.size() == 6);
	CHECK(r.options[0].id == t_verbose);
	CHECK(r.options[1].id == t_output);
	CHECK(r.options[1].value == "out");
	CHECK(r.options[2].value == "inc");
	CHECK(r.options[3].value == "dir");
	CHECK(r.options[4].id == t_lib);
	CHECK(r.options[4].value == "m");
	CHECK(r.options[5].id == t_std);
	CHECK(r.options[5].value == "c17");
	CHECK(r.operands == words({ "a.c", "b.c" }));
	CHECK(r.remainder.empty());
	CHECK(r.option_words == words({ "-v", "-o", "out", "-Iinc", "-I", "dir", "-lm", "--std=c17" }));
}

TEST_CASE("stop_at_first: the first operand starts the remainder, verbatim")
{
	const cli::table t = make_table(cli::operand_rule::stop_at_first);
	const cli::result r = cli::parse(t, words({ "-v", "tool.mad", "-o", "x", "--bogus" }));
	REQUIRE(r.ok());
	CHECK(r.options.size() == 1);
	CHECK(r.operands.empty());
	CHECK(r.remainder == words({ "tool.mad", "-o", "x", "--bogus" }));
}

TEST_CASE("-- ends the options; - is an operand")
{
	const cli::table t = make_table(cli::operand_rule::permute);
	const cli::result r = cli::parse(t, words({ "-", "--", "-v" }));
	REQUIRE(r.ok());
	CHECK(r.options.empty());
	CHECK(r.operands == words({ "-" }));
	CHECK(r.remainder == words({ "-v" }));
}

TEST_CASE("the longest glued spelling wins; a whole word beats a glued one")
{
	const cli::table t = make_table(cli::operand_rule::permute);
	const cli::result r = cli::parse(t, words({ "-Wall", "-Werror=x", "-Werror", "-Wl,-rpath,/x",
						    "-O", "-O2" }));
	REQUIRE(r.ok());
	REQUIRE(r.options.size() == 6);
	CHECK(r.options[0].id == t_warn);
	CHECK(r.options[0].value == "all");
	CHECK(r.options[1].id == t_werror);
	CHECK(r.options[1].value == "=x");
	CHECK(r.options[2].id == t_werror);
	CHECK(r.options[2].value == "");
	CHECK(r.options[3].id == t_link);
	CHECK(r.options[3].value == "-rpath,/x");
	CHECK(r.options[4].value == "");
	CHECK(r.options[5].value == "2");
	CHECK(std::string(r.options[5].spelled) == "-O");
	CHECK(r.options[5].word == "-O2");
}

TEST_CASE("value forms; a value that starts with a dash is a value (gcc)")
{
	const cli::table t = make_table(cli::operand_rule::permute);
	const cli::result r = cli::parse(t, words({ "--run", "--run=f", "--long=a", "--long", "b",
						    "-o", "-x", "-I", "-dir" }));
	REQUIRE(r.ok());
	REQUIRE(r.options.size() == 6);
	CHECK(r.options[0].value == "");
	CHECK(r.options[0].word == "--run");
	CHECK(r.options[1].value == "f");
	CHECK(r.options[2].value == "a");
	CHECK(r.options[3].value == "b");
	CHECK(r.options[4].value == "-x");
	CHECK(r.options[5].value == "-dir");
}

TEST_CASE("an option that ends the options")
{
	const cli::table t = make_table(cli::operand_rule::permute);
	const cli::result r = cli::parse(t, words({ "--relayout=img", "a.log", "-v" }));
	REQUIRE(r.ok());
	CHECK(r.options.size() == 1);
	CHECK(r.remainder == words({ "a.log", "-v" }));
}

TEST_CASE("errors: unknown, did you mean, missing, unexpected")
{
	const cli::table t = make_table(cli::operand_rule::permute);
	cli::result r = cli::parse(t, words({ "-fbogus" }));
	CHECK(r.error == cli::error_kind::unknown_option);
	CHECK(r.message == "unrecognized command-line option '-fbogus'");

	r = cli::parse(t, words({ "--verbos" }));
	CHECK(r.error == cli::error_kind::unknown_option);
	CHECK(r.message == "unrecognized command-line option '--verbos'; did you mean '--verbose'?");

	r = cli::parse(t, words({ "--stdd=c17" }));
	CHECK(r.message == "unrecognized command-line option '--stdd=c17'; did you mean '--std='?");

	r = cli::parse(t, words({ "-o" }));
	CHECK(r.error == cli::error_kind::missing_value);
	CHECK(r.message == "missing argument to '-o'");

	r = cli::parse(t, words({ "-v", "-l" }));
	CHECK(r.error == cli::error_kind::missing_value);
	CHECK(r.message == "missing argument to '-l'");

	r = cli::parse(t, words({ "--verbose=1" }));
	CHECK(r.error == cli::error_kind::unexpected_value);
	CHECK(r.message == "option '--verbose' doesn't allow an argument");
}

TEST_CASE("bundling, when the table asks for it")
{
	cli::table t = make_table(cli::operand_rule::permute);
	t.bundling = true;
	cli::result r = cli::parse(t, words({ "-vq", "-vofile", "-vo", "next" }));
	REQUIRE(r.ok());
	REQUIRE(r.options.size() == 6);
	CHECK(r.options[0].id == t_verbose);
	CHECK(r.options[1].id == t_quiet);
	CHECK(r.options[3].id == t_output);
	CHECK(r.options[3].value == "file");
	CHECK(r.options[5].value == "next");
	CHECK(r.option_words == words({ "-vq", "-vofile", "-vo", "next" }));

	r = cli::parse(t, words({ "-vz" }));
	CHECK(r.error == cli::error_kind::unknown_option);

	t.bundling = false;
	r = cli::parse(t, words({ "-vq" }));
	CHECK(r.error == cli::error_kind::unknown_option);
}

TEST_CASE("--no-NAME for a negatable flag; has() reads the last word")
{
	const cli::table t = make_table(cli::operand_rule::permute);
	cli::result r = cli::parse(t, words({ "--colour", "--no-colour" }));
	REQUIRE(r.ok());
	CHECK(r.options.size() == 2);
	CHECK(r.options[1].negated);
	CHECK_FALSE(r.has(t_colour));
	CHECK(r.count(t_colour) == 1);

	r = cli::parse(t, words({ "--no-verbose" }));
	CHECK(r.error == cli::error_kind::unknown_option);
}

TEST_CASE("the caller's operand rule")
{
	cli::table t = make_table(cli::operand_rule::caller);
	t.operand_continues = c_sources_continue;
	cli::result r = cli::parse(t, words({ "x.c", "-v", "y" }));
	REQUIRE(r.ok());
	CHECK(r.operands == words({ "x.c", "y" }));
	CHECK(r.options.size() == 1);

	r = cli::parse(t, words({ "s.mad", "-v" }));
	REQUIRE(r.ok());
	CHECK(r.options.empty());
	CHECK(r.remainder == words({ "s.mad", "-v" }));
}

TEST_CASE("match_word and find_row")
{
	const cli::table t = make_table(cli::operand_rule::permute);
	const cli::match m = cli::match_word(t, "-O2");
	REQUIRE(m.option != NULL);
	CHECK(m.option->id == t_opt);
	CHECK(m.has_value);
	CHECK(m.value == "2");
	CHECK(cli::match_word(t, "file.c").option == NULL);
	CHECK(cli::match_word(t, "-").option == NULL);
	REQUIRE(cli::find_row(t, t_std) != NULL);
	CHECK(std::string(cli::find_row(t, t_std)->name) == "std");
	CHECK(cli::find_row(t, 999) == NULL);
}

TEST_CASE("result accessors")
{
	const cli::table t = make_table(cli::operand_rule::permute);
	const cli::result r = cli::parse(t, words({ "-v", "-v", "-Ia", "-Ib", "-O3" }));
	REQUIRE(r.ok());
	CHECK(r.count(t_verbose) == 2);
	CHECK(r.has(t_verbose));
	CHECK_FALSE(r.has(t_quiet));
	CHECK(r.values(t_include) == words({ "a", "b" }));
	CHECK(r.integer(t_opt, 0) == 3);
	CHECK(r.integer(t_quiet, 7) == 7);
	CHECK(r.boolean(t_verbose, false));
	CHECK_FALSE(r.boolean(t_quiet, false));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bash scripts/remote_build.sh sync build`, then restore `hello.cpp` (Global Constraints), then `ssh -p 2299 dev@localhost 'cd /workspace/madc && make -C src ../bin/test_cli_options'`.
Expected: FAIL to compile, with `libmadc/cli_options.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/libmadc/cli_options.h`:

```cpp
#ifndef __LIBMADC_CLI_OPTIONS_H
#define __LIBMADC_CLI_OPTIONS_H 1

// libmadc/cli_options.h — THE command-line option reader (design:
// docs/plans/2026-10-16-cli-options-design.md). A program declares its
// options once, as a table of rows; parse() reads a command line against the
// table, and the writers derive the help, a JSON description and shell
// completion scripts from the same rows. madc's own command line
// (src/madc_cli.cpp) is one table; the dialect faces are others.
//
// A table is constant data: rows are aggregates of literals, so a program's
// table costs nothing to build (madc reads its own at every start).
//
// THREAD-SAFETY CONTRACT: a table is immutable and may be shared across
// threads; parse() and the writers only read it. parse() is reentrant: it
// reads its arguments, the response files they name and, when asked, the
// environment through the caller's function, and returns a new result, a
// value the caller owns. An occurrence's `spelled` points into the table:
// keep the table alive while reading a result. No global state.

#include <cstddef>
#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "madcdis/istring.h"

namespace madc {
namespace cli {

// How one spelling takes its value.
enum class value_form : uint8_t
{
	none,			// -c, --verbose
	separate,		// -o FILE: the next argument, whatever it holds
	joined,			// -lNAME: glued on, not empty
	prefix,			// -O, -O2, -Werror=x: glued on, may be empty
	joined_or_separate,	// -IDIR or -I DIR
	equals,			// --std=VALUE
	optional_equals,	// --run-frozen or --run-frozen=FILE
	equals_or_separate	// --output=FILE or --output FILE (getopt_long)
};

// What a value must look like; anything else is a parse error.
enum class value_type : uint8_t
{
	text,			// anything (the default)
	integer,		// a whole decimal number, optionally signed
	number,			// a decimal number
	boolean,		// yes/no, true/false, on/off, 1/0 (any case)
	path			// a path, stored in generic form (host_path_generic)
};

// A row's flags: what the reader does around it.
enum row_flag : uint32_t
{
	rf_none		= 0,
	rf_ends_options	= 1u << 0,	// every argument after it is the remainder
	rf_hidden	= 1u << 1,	// read; never shown in help or completions
	rf_negatable	= 1u << 2,	// a "--name" flag also reads "--no-name"
	rf_required	= 1u << 3,	// a parse that never sets it fails
	rf_doc		= 1u << 4	// a help line only (an operand form, an
					// environment variable): never read
};

const size_t max_spellings = 4;

struct spelling
{
	const char *text;	// "-o", "--std": an equals form adds the '='
	value_form form;
};

// One option. An aggregate of literals: fields left out of an initializer are
// zero — no further spelling, no metavar, text type, no default.
struct row
{
	int id;				// the caller's code (an enum value)
	const char *name;		// a stable key: --help=json, sys.flags
	spelling spellings[max_spellings];	// the first is the primary one
	const char *metavar;		// the value's name in help ("<file>")
	unsigned section;		// index into table::sections
	const char *help;		// '\n' separates paragraphs; the writer wraps each
	const char *usage;		// help's left column, when the spellings can't say it
	uint32_t flags;			// row_flag bits
	uint32_t tags;			// the caller's own bits; the reader never reads them
	value_type type;
	const char *const *choices;	// NULL-terminated; non-NULL: the value is one of them
	const char *default_value;	// NULL: none
	const char *env;		// an environment variable that supplies the value
	const char *config_key;		// its key in the program's config file
};

struct section
{
	const char *title;
	const char *note;		// printed under the title, before its rows
};

enum class group_rule : uint8_t
{
	exclusive,			// at most one of the ids
	requires_rest			// ids[0] given: every other id must be too
};

const size_t max_group_ids = 8;

struct group
{
	group_rule rule;
	size_t nids;
	int ids[max_group_ids];
};

// Whether options may follow an operand.
enum class operand_rule : uint8_t
{
	permute,		// yes, anywhere (GNU getopt, gcc)
	stop_at_first,		// no: the first operand starts the remainder (POSIX)
	caller			// table::operand_continues decides at each operand
};

struct table;
struct result;

// At operand words[at]: true = options may still follow it.
typedef bool (*operand_policy)(const table &t, const result &so_far,
			       const std::vector<std::string> &words, size_t at);

struct table
{
	const char *program;		// "madc": the usage line, completions
	const char *usage;		// the usage line after the program name
	const char *description;	// help's prologue (NULL: none)
	const char *epilog;		// help's epilogue (NULL: none)
	const section *sections;
	size_t nsections;
	const row *rows;
	size_t nrows;
	const group *groups;
	size_t ngroups;
	bool bundling;			// -abc reads as -a -b -c
	bool response_files;		// @file expands in place (gcc, MSVC)
	operand_rule operands;
	operand_policy operand_continues;	// operand_rule::caller only
	const void *context;		// the policy's own data
};

enum class value_source : uint8_t { command_line, environment, config, default_value };

struct occurrence
{
	int id;
	std::string value;		// empty for a flag
	std::string word;		// the argument as written: "-O2", "--std=c17", "-o"
	const char *spelled;		// the spelling it matched (into the table); NULL off the command line
	bool negated;			// read through --no-<name>
	value_source source;
};

enum class error_kind : uint8_t
{
	none,
	unknown_option,		// -fbogus
	missing_value,		// -o at the end of the line
	unexpected_value,	// --verbose=1
	bad_value,		// not of the row's type, or not among its choices
	conflict,		// two options of an exclusive group
	missing_required,	// a required option, or a group's requirement
	response_file		// an @file that cannot be expanded
};

struct result
{
	std::vector<occurrence> options;	// the command line in order, then the other sources
	std::vector<std::string> operands;	// operands read while options could still follow
	std::vector<std::string> remainder;	// every argument after the options ended, verbatim
	std::vector<std::string> option_words;	// the words the options consumed, @file expanded
	error_kind error;
	std::string message;			// the diagnostic sentence, without a program prefix

	result() : error(error_kind::none) {}
	bool ok() const { return error == error_kind::none; }
	const occurrence *last(int id) const;		// the last occurrence, negated or not
	bool has(int id) const;				// it exists and is not negated
	size_t count(int id) const;			// non-negated occurrences (-vvv)
	std::vector<std::string> values(int id) const;	// their values, in order
	long long integer(int id, long long fallback) const;
	double number(int id, double fallback) const;
	bool boolean(int id, bool fallback) const;
};

// THE reader.
result parse(const table &t, const std::vector<std::string> &args);

// What one word names as an option.
struct match
{
	const row *option;		// NULL: no option
	const spelling *spelled;
	bool has_value;			// the word carries its value (glued or after '=')
	std::string value;
	bool negated;
	match() : option(NULL), spelled(NULL), has_value(false), negated(false) {}
};
match match_word(const table &t, const std::string &word);

// The row of `id`; NULL when the table has none.
const row *find_row(const table &t, int id);

} // namespace cli
} // namespace madc

#endif
```

- [ ] **Step 4: Write the reader**

Create `src/cli_options.cpp`:

```cpp
// src/cli_options.cpp — THE command-line option reader. Contract and
// thread-safety: include/libmadc/cli_options.h; design:
// docs/plans/2026-10-16-cli-options-design.md.

#include "libmadc/cli_options.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace madc {
namespace cli {

namespace {

bool takes_bare_word(value_form f)
{
	return f == value_form::none || f == value_form::separate
		|| f == value_form::prefix || f == value_form::joined_or_separate
		|| f == value_form::optional_equals
		|| f == value_form::equals_or_separate;
}

bool takes_equals(value_form f)
{
	return f == value_form::equals || f == value_form::optional_equals
		|| f == value_form::equals_or_separate;
}

bool takes_glued(value_form f)
{
	return f == value_form::joined || f == value_form::prefix
		|| f == value_form::joined_or_separate;
}

// A bare spelling of this form reads its value from the next word.
bool value_follows(value_form f)
{
	return f == value_form::separate || f == value_form::joined_or_separate
		|| f == value_form::equals_or_separate;
}

bool starts_with(const std::string &s, const char *p)
{
	return s.compare(0, strlen(p), p) == 0;
}

std::string quoted(const std::string &s)
{
	return "'" + s + "'";
}

bool fail(result &r, error_kind kind, const std::string &message)
{
	r.error = kind;
	r.message = message;
	return false;
}

// Every spelling a word could be read as, in table order; f returns true
// to stop.
template <typename F>
void each_spelling(const table &t, F f)
{
	for ( size_t r = 0; r < t.nrows; r++ )
	{
		const row &rw = t.rows[r];
		if ( rw.flags & rf_doc )
			continue;
		for ( size_t s = 0; s < max_spellings && rw.spellings[s].text; s++ )
			if ( f(rw, rw.spellings[s]) )
				return;
	}
}

// Optimal-string-alignment distance: insertions, deletions, substitutions
// and adjacent transpositions each cost one.
size_t edit_distance(const std::string &a, const std::string &b)
{
	const size_t n = a.size(), m = b.size();
	std::vector<std::vector<size_t> > d(n + 1, std::vector<size_t>(m + 1));
	for ( size_t i = 0; i <= n; i++ )
		d[i][0] = i;
	for ( size_t j = 0; j <= m; j++ )
		d[0][j] = j;
	for ( size_t i = 1; i <= n; i++ )
		for ( size_t j = 1; j <= m; j++ )
		{
			const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
			size_t v = std::min(std::min(d[i - 1][j] + 1, d[i][j - 1] + 1),
					    d[i - 1][j - 1] + cost);
			if ( i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] )
				v = std::min(v, d[i - 2][j - 2] + 1);
			d[i][j] = v;
		}
	return d[n][m];
}

// How far a suggestion may be, after gcc's spellcheck: none for one-letter
// words; a third of the longer length when the lengths are close, else a
// little more.
size_t edit_cutoff(size_t goal, size_t candidate)
{
	const size_t hi = std::max(goal, candidate), lo = std::min(goal, candidate);
	if ( hi <= 1 )
		return 0;
	if ( hi - lo <= 1 )
		return std::max(hi / 3, (size_t)1);
	return (hi + 2) / 3;
}

// The spelling nearest `word`, as a user would type it; "" when none is
// near enough. A NAME=VALUE word is compared by its "NAME=".
std::string nearest_spelling(const table &t, const std::string &word)
{
	const size_t eq = word.find('=');
	const std::string goal = eq == std::string::npos ? word : word.substr(0, eq + 1);
	std::string best;
	size_t best_d = (size_t)-1;
	each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
		if ( rw.flags & rf_hidden )
			return false;
		std::string cand(sp.text);
		if ( sp.form == value_form::equals )
			cand += '=';
		const size_t d = edit_distance(goal, cand);
		if ( d <= edit_cutoff(goal.size(), cand.size()) && d < best_d )
		{
			best_d = d;
			best = cand;
		}
		return false;
	});
	return best;
}

// Why `word` names no option: a flag given a value, or nothing at all.
bool fail_unknown(const table &t, const std::string &word, result &r)
{
	const size_t eq = word.find('=');
	if ( eq != std::string::npos )
	{
		const std::string head = word.substr(0, eq);
		bool flag = false;
		each_spelling(t, [&](const row &, const spelling &sp) -> bool {
			flag = sp.form == value_form::none && head == sp.text;
			return flag;
		});
		if ( flag )
			return fail(r, error_kind::unexpected_value,
				    "option " + quoted(head) + " doesn't allow an argument");
	}
	std::string message = "unrecognized command-line option " + quoted(word);
	const std::string near = nearest_spelling(t, word);
	if ( !near.empty() )
		message += "; did you mean " + quoted(near) + "?";
	return fail(r, error_kind::unknown_option, message);
}

bool operand_continues(const table &t, const result &r,
		       const std::vector<std::string> &words, size_t at)
{
	switch ( t.operands )
	{
	case operand_rule::permute:
		return true;
	case operand_rule::stop_at_first:
		return false;
	case operand_rule::caller:
		return t.operand_continues ? t.operand_continues(t, r, words, at) : true;
	}
	return true;
}

bool parse_boolean(const std::string &s, bool &out)
{
	std::string l;
	for ( size_t i = 0; i < s.size(); i++ )
		l += (char)tolower((unsigned char)s[i]);
	if ( l == "1" || l == "yes" || l == "true" || l == "on" )
	{
		out = true;
		return true;
	}
	if ( l == "0" || l == "no" || l == "false" || l == "off" )
	{
		out = false;
		return true;
	}
	return false;
}

occurrence command_line_occurrence(const row &rw, const spelling &sp,
				   const std::string &word, bool negated)
{
	occurrence o;
	o.id = rw.id;
	o.word = word;
	o.spelled = sp.text;
	o.negated = negated;
	o.source = value_source::command_line;
	return o;
}

// One pass over a command line. The words are a copy: a response file
// (Task 2) splices its words in place.
class reader
{
public:
	reader(const table &t, const std::vector<std::string> &args, result &r)
		: _t(t), _words(args), _r(r) {}
	bool run();

private:
	// words[k] exists and is ready to read.
	bool fetch(size_t k) { return k < _words.size(); }
	bool read_option(size_t &i);
	bool read_bundle(size_t &i);
	bool take_value(const match &m, const std::string &word, size_t &i,
			occurrence &o);

	const table &_t;
	std::vector<std::string> _words;
	result &_r;
};

bool reader::run()
{
	bool reading = true;
	size_t i = 0;
	for ( ;; )
	{
		if ( !reading )
		{
			// The remainder is the program's own: verbatim, never expanded.
			_r.remainder.insert(_r.remainder.end(), _words.begin() + i, _words.end());
			return true;
		}
		if ( !fetch(i) )
			return _r.ok();
		const std::string word = _words[i];
		if ( word == "--" )
		{
			reading = false;
			i++;
			continue;
		}
		if ( word.size() < 2 || word[0] != '-' )
		{
			if ( !operand_continues(_t, _r, _words, i) )
			{
				reading = false;
				continue;
			}
			_r.operands.push_back(word);
			i++;
			continue;
		}
		if ( !read_option(i) )
			return false;
		const row *rw = find_row(_t, _r.options.back().id);
		if ( rw && (rw->flags & rf_ends_options) )
			reading = false;
	}
}

bool reader::read_option(size_t &i)
{
	const std::string word = _words[i];
	const match m = match_word(_t, word);
	if ( !m.option )
	{
		if ( _t.bundling && word[1] != '-' )
			return read_bundle(i);
		return fail_unknown(_t, word, _r);
	}
	occurrence o = command_line_occurrence(*m.option, *m.spelled, word, m.negated);
	_r.option_words.push_back(word);
	if ( !take_value(m, word, i, o) )
		return false;
	_r.options.push_back(o);
	i++;
	return true;
}

// The occurrence's value: the one the word carries, else the next word for a
// form that reads one (i moves onto it).
bool reader::take_value(const match &m, const std::string &word, size_t &i,
			occurrence &o)
{
	const value_form f = m.spelled->form;
	if ( m.has_value )
	{
		if ( f == value_form::joined && m.value.empty() )
			return fail(_r, error_kind::missing_value, "missing argument to " + quoted(word));
		o.value = m.value;
		return true;
	}
	if ( !value_follows(f) )
		return true;
	if ( !fetch(i + 1) )
		return _r.ok() ? fail(_r, error_kind::missing_value,
				      "missing argument to " + quoted(word))
			       : false;
	o.value = _words[i + 1];
	_r.option_words.push_back(o.value);
	i++;
	return true;
}

// -abc over one-letter spellings: -a -b -c. A letter that takes a value takes
// the rest of the word, else the next word (getopt).
bool reader::read_bundle(size_t &i)
{
	const std::string word = _words[i];
	std::vector<occurrence> got;
	std::vector<std::string> consumed(1, word);
	for ( size_t k = 1; k < word.size(); k++ )
	{
		const std::string letter = std::string("-") + word[k];
		const row *owner = NULL;
		const spelling *sp = NULL;
		each_spelling(_t, [&](const row &rw, const spelling &s) -> bool {
			if ( letter != s.text )
				return false;
			owner = &rw;
			sp = &s;
			return true;
		});
		if ( !sp || takes_equals(sp->form) )
			return fail_unknown(_t, word, _r);
		occurrence o = command_line_occurrence(*owner, *sp, word, false);
		if ( sp->form == value_form::none )
		{
			got.push_back(o);
			continue;
		}
		o.value = word.substr(k + 1);
		if ( o.value.empty() && value_follows(sp->form) )
		{
			if ( !fetch(i + 1) )
				return _r.ok() ? fail(_r, error_kind::missing_value,
						      "missing argument to " + quoted(letter))
					       : false;
			o.value = _words[++i];
			consumed.push_back(o.value);
		}
		else if ( o.value.empty() && sp->form == value_form::joined )
			return fail(_r, error_kind::missing_value, "missing argument to " + quoted(letter));
		got.push_back(o);
		break;
	}
	_r.options.insert(_r.options.end(), got.begin(), got.end());
	_r.option_words.insert(_r.option_words.end(), consumed.begin(), consumed.end());
	i++;
	return true;
}

} // namespace

match match_word(const table &t, const std::string &word)
{
	match m;
	if ( word.size() < 2 || word[0] != '-' )
		return m;
	// 1. The whole word is a spelling.
	each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
		if ( !takes_bare_word(sp.form) || word != sp.text )
			return false;
		m.option = &rw;
		m.spelled = &sp;
		return true;
	});
	if ( m.option )
		return m;
	// 2. NAME=VALUE.
	const size_t eq = word.find('=');
	if ( eq != std::string::npos )
	{
		each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
			if ( !takes_equals(sp.form) || word.compare(0, eq, sp.text) != 0 )
				return false;
			m.option = &rw;
			m.spelled = &sp;
			m.has_value = true;
			m.value = word.substr(eq + 1);
			return true;
		});
		if ( m.option )
			return m;
	}
	// 3. A glued value: the longest spelling the word starts with.
	size_t longest = 0;
	each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
		const size_t n = strlen(sp.text);
		if ( takes_glued(sp.form) && n > longest && starts_with(word, sp.text) )
		{
			longest = n;
			m.option = &rw;
			m.spelled = &sp;
		}
		return false;
	});
	if ( m.option )
	{
		m.has_value = true;
		m.value = word.substr(longest);
		return m;
	}
	// 4. --no-NAME for a negatable flag.
	if ( starts_with(word, "--no-") )
	{
		const std::string positive = "--" + word.substr(5);
		each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
			if ( !(rw.flags & rf_negatable) || sp.form != value_form::none
			     || positive != sp.text )
				return false;
			m.option = &rw;
			m.spelled = &sp;
			m.negated = true;
			return true;
		});
	}
	return m;
}

const row *find_row(const table &t, int id)
{
	for ( size_t r = 0; r < t.nrows; r++ )
		if ( t.rows[r].id == id && !(t.rows[r].flags & rf_doc) )
			return &t.rows[r];
	return NULL;
}

result parse(const table &t, const std::vector<std::string> &args)
{
	result r;
	reader(t, args, r).run();
	return r;
}

const occurrence *result::last(int id) const
{
	for ( size_t k = options.size(); k-- > 0; )
		if ( options[k].id == id )
			return &options[k];
	return NULL;
}

bool result::has(int id) const
{
	const occurrence *o = last(id);
	return o && !o->negated;
}

size_t result::count(int id) const
{
	size_t n = 0;
	for ( size_t k = 0; k < options.size(); k++ )
		if ( options[k].id == id && !options[k].negated )
			n++;
	return n;
}

std::vector<std::string> result::values(int id) const
{
	std::vector<std::string> out;
	for ( size_t k = 0; k < options.size(); k++ )
		if ( options[k].id == id && !options[k].negated )
			out.push_back(options[k].value);
	return out;
}

long long result::integer(int id, long long fallback) const
{
	const occurrence *o = last(id);
	if ( !o || o->negated || o->value.empty() )
		return fallback;
	return strtoll(o->value.c_str(), NULL, 10);
}

double result::number(int id, double fallback) const
{
	const occurrence *o = last(id);
	if ( !o || o->negated || o->value.empty() )
		return fallback;
	return strtod(o->value.c_str(), NULL);
}

bool result::boolean(int id, bool fallback) const
{
	const occurrence *o = last(id);
	if ( !o )
		return fallback;
	if ( o->negated )
		return false;
	if ( o->value.empty() )
		return true;		// a flag
	bool b;
	return parse_boolean(o->value, b) ? b : fallback;
}

} // namespace cli
} // namespace madc
```

Add `cli_options.o` to `CORE_OFILES` in `src/Makefile:135`, directly before `madc_capabilities.o`:

```make
... file_kinds.o file_kind_editing.o cli_options.o madc_capabilities.o madc_console.o ...
```

Add the two files to the `CORE` list in `scripts/check-istring-fields.sh:41-46`. Append to the last line:

```bash
include/madc_mangle.h include/madc_type_spelling.h include/spelling_delim.h
src/cli_options.cpp include/libmadc/cli_options.h"
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `bash scripts/remote_build.sh sync build`, restore `hello.cpp`, then `ssh -p 2299 dev@localhost 'cd /workspace/madc && make -C src ../bin/test_cli_options && bin/test_cli_options'`.
Expected: `[doctest] Status: SUCCESS!` with every test case passing.

Record the istring baseline for the new files: `ssh -p 2299 dev@localhost 'cd /workspace/madc && bash scripts/check-istring-fields.sh --record'`, then copy it back: `scp -P 2299 dev@localhost:/workspace/madc/scripts/istring_keep.tsv scripts/istring_keep.tsv`. Confirm `git diff scripts/istring_keep.tsv` adds only the two new rows (the count is the engine's built-text `std::string`s: values, words, messages).

- [ ] **Step 6: Tier 1 + Tier 2, gates, commit**

Run: `TESTS='testcli_*' bash scripts/remote_build.sh sync build fix`, then the static gate loop. Expected: all green; the reader has no caller yet, so madc's behaviour is unchanged.
Stage `include/libmadc/cli_options.h src/cli_options.cpp tests/unit/test_cli_options.cpp src/Makefile scripts/check-istring-fields.sh scripts/istring_keep.tsv` and dispatch the `commit-writer` agent. Pointers: `git diff --cached`; the unit test file; spec section 4.2. No CHANGELOG entry: the reader has no user-visible effect yet.

---
### Task 2: Response files (`@file`)

**Files:**
- Modify: `src/cli_options.cpp` (the `reader` class, its `fetch`, a new `split_response_text`)
- Modify: `tests/unit/test_cli_options.cpp`

**Interfaces:**
- Consumes: Task 1's `table::response_files`, `error_kind::response_file` and `result::option_words`.
- Produces: `@file` expansion inside `parse()`. There is no new API.

- [ ] **Step 1: Find gcc's answer (the oracle)**

Write `tmp/cli_rsp.txt` locally (Write tool) with this one line:

```
-DA="x y" -DB='p q' -DC=a\ b -DD=a\\b
```

Copy it to the container and ask gcc:

Run: `scp -P 2299 tmp/cli_rsp.txt dev@localhost:/workspace/madc/tmp/cli_rsp.txt`
Run: `ssh -p 2299 dev@localhost 'cd /workspace/madc && gcc -dM -E @tmp/cli_rsp.txt -x c /dev/null | grep -E "^#define (A|B|C|D) " | sort'`
Expected:
```
#define A x y
#define B p q
#define C a b
#define D a\b
```
These are libiberty's `buildargv` rules: quotes group, and a backslash escapes the next character everywhere, inside quotes too. The unit test pins the same four cases.

- [ ] **Step 2: Write the failing tests**

In `tests/unit/test_cli_options.cpp`, add these includes beside the others:

```cpp
#include <fstream>
#include <sstream>
#include <unistd.h>
```

Inside the anonymous namespace, before its closing `} // namespace`, add:

```cpp
// Fixture files follow the unit-test convention: /tmp/madc_<what>_<pid>.
std::string fixture_path(const char *tag)
{
	std::ostringstream p;
	p << "/tmp/madc_cli_" << tag << "_" << (long)getpid();
	return p.str();
}

std::string write_fixture(const char *tag, const std::string &body)
{
	const std::string path = fixture_path(tag);
	std::ofstream f(path.c_str(), std::ios::binary);
	f << body;
	return path;
}
```

Append these test cases at the end of the file:

```cpp
TEST_CASE("@file expands in place with gcc's quoting, option values included")
{
	cli::table t = make_table(cli::operand_rule::permute);
	t.response_files = true;
	const std::string rsp = write_fixture("rsp",
		"-v -o 'a b' \"-Ic d\" -Ie\\ f -Ig\\\\h\n-l\\m x.c \"\"\n");
	const std::string out = write_fixture("out", "result.o");
	std::vector<std::string> args;
	args.push_back("@" + rsp);
	args.push_back("-o");
	args.push_back("@" + out);
	args.push_back("y.c");
	const cli::result r = cli::parse(t, args);
	REQUIRE(r.ok());
	REQUIRE(r.options.size() == 7);
	CHECK(r.options[1].value == "a b");
	CHECK(r.options[2].value == "c d");
	CHECK(r.options[3].value == "e f");
	CHECK(r.options[4].value == "g\\h");
	CHECK(r.options[5].value == "m");
	CHECK(r.options[6].value == "result.o");
	CHECK(r.operands == words({ "x.c", "", "y.c" }));
	CHECK(r.option_words == words({ "-v", "-o", "a b", "-Ic d", "-Ie f", "-Ig\\h", "-lm",
					"-o", "result.o" }));
	unlink(rsp.c_str());
	unlink(out.c_str());
}

TEST_CASE("@file: an unopenable name stays a word; a self-reference and a directory are errors")
{
	cli::table t = make_table(cli::operand_rule::permute);
	t.response_files = true;
	cli::result r = cli::parse(t, words({ "@/nonexistent/madc_cli_rsp" }));
	REQUIRE(r.ok());
	CHECK(r.operands == words({ "@/nonexistent/madc_cli_rsp" }));

	const std::string self = fixture_path("self");
	write_fixture("self", "@" + self + "\n");
	r = cli::parse(t, std::vector<std::string>(1, "@" + self));
	CHECK(r.error == cli::error_kind::response_file);
	CHECK(r.message == "too many @-files encountered");
	unlink(self.c_str());

	r = cli::parse(t, words({ "@/tmp" }));
	CHECK(r.error == cli::error_kind::response_file);
	CHECK(r.message == "@-file refers to a directory: '/tmp'");
}

TEST_CASE("@file: off unless the table asks; never inside the remainder")
{
	const std::string rsp = write_fixture("rest", "-v\n");
	std::vector<std::string> args;
	args.push_back("tool.mad");
	args.push_back("@" + rsp);
	cli::table t = make_table(cli::operand_rule::stop_at_first);
	t.response_files = true;
	cli::result r = cli::parse(t, args);
	REQUIRE(r.ok());
	CHECK(r.options.empty());
	CHECK(r.remainder == args);

	const cli::table off = make_table(cli::operand_rule::permute);
	const std::vector<std::string> just(1, "@" + rsp);
	r = cli::parse(off, just);
	REQUIRE(r.ok());
	CHECK(r.operands == just);
	unlink(rsp.c_str());
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `bash scripts/remote_build.sh sync build`, restore `hello.cpp`, then `ssh -p 2299 dev@localhost 'cd /workspace/madc && make -C src ../bin/test_cli_options && bin/test_cli_options'`.
Expected: the three new test cases FAIL. The first sees `@/tmp/madc_cli_rsp_<pid>` as an operand, and the self-reference parses as ok.

- [ ] **Step 4: Implement**

In `src/cli_options.cpp`, add these includes beside the others:

```cpp
#include <fstream>
#include <sstream>
#include <sys/stat.h>
```

Inside the anonymous namespace, directly after `parse_boolean`, add:

```cpp
// libiberty's limit (expandargv): a file that names itself stops here.
const unsigned response_file_limit = 2000;

// A response file's words, by libiberty's buildargv (gcc's @file): blanks
// separate words; '...' and "..." quote; a backslash makes the next
// character literal everywhere, inside quotes too; "" is an empty word.
std::vector<std::string> split_response_text(const std::string &s)
{
	std::vector<std::string> out;
	size_t i = 0;
	for ( ;; )
	{
		while ( i < s.size() && isspace((unsigned char)s[i]) )
			i++;
		if ( i >= s.size() )
			return out;
		std::string arg;
		bool squote = false, dquote = false, bsquote = false;
		for ( ; i < s.size(); i++ )
		{
			const char c = s[i];
			if ( isspace((unsigned char)c) && !squote && !dquote && !bsquote )
				break;
			if ( bsquote )
			{
				bsquote = false;
				arg += c;
			}
			else if ( c == '\\' )
				bsquote = true;
			else if ( squote )
			{
				if ( c == '\'' )
					squote = false;
				else
					arg += c;
			}
			else if ( dquote )
			{
				if ( c == '"' )
					dquote = false;
				else
					arg += c;
			}
			else if ( c == '\'' )
				squote = true;
			else if ( c == '"' )
				dquote = true;
			else
				arg += c;
		}
		out.push_back(arg);
	}
}
```

In the `reader` class, replace

```cpp
	reader(const table &t, const std::vector<std::string> &args, result &r)
		: _t(t), _words(args), _r(r) {}
```

with

```cpp
	reader(const table &t, const std::vector<std::string> &args, result &r)
		: _t(t), _words(args), _r(r), _expansions(0) {}
```

Replace

```cpp
	// words[k] exists and is ready to read.
	bool fetch(size_t k) { return k < _words.size(); }
```

with

```cpp
	// words[k] exists and is ready to read (an @file there expanded).
	bool fetch(size_t k);
```

Replace

```cpp
	const table &_t;
	std::vector<std::string> _words;
	result &_r;
};
```

with

```cpp
	const table &_t;
	std::vector<std::string> _words;
	result &_r;
	unsigned _expansions;	// @files expanded so far
};

// words[k] exists, ready to read. A "@file" there is first replaced by the
// file's words — repeatedly, so one file may name another (gcc's
// expandargv). A name that cannot be opened stays as written (gcc); a
// directory, or a file past response_file_limit expansions, is an error.
bool reader::fetch(size_t k)
{
	while ( _t.response_files && k < _words.size()
		&& _words[k].size() > 1 && _words[k][0] == '@' )
	{
		const std::string path = _words[k].substr(1);
		struct stat st;
		if ( stat(path.c_str(), &st) != 0 )
			return true;
		if ( S_ISDIR(st.st_mode) )
			return fail(_r, error_kind::response_file,
				    "@-file refers to a directory: " + quoted(path));
		std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
		if ( !in )
			return true;
		if ( ++_expansions > response_file_limit )
			return fail(_r, error_kind::response_file, "too many @-files encountered");
		std::ostringstream text;
		text << in.rdbuf();
		const std::vector<std::string> got = split_response_text(text.str());
		_words.erase(_words.begin() + k);
		_words.insert(_words.begin() + k, got.begin(), got.end());
	}
	return k < _words.size();
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: the Step 3 commands.
Expected: `[doctest] Status: SUCCESS!`

- [ ] **Step 6: Tier 1 + Tier 2, gates, commit**

Run: `TESTS='testcli_*' bash scripts/remote_build.sh sync build fix`, then the static gate loop. Expected: green.
Stage `src/cli_options.cpp tests/unit/test_cli_options.cpp` and dispatch `commit-writer`. Pointers: `git diff --cached`; the unit test; the Step 1 gcc output as the oracle.

---

### Task 3: Typed values, choices, groups, required options

**Files:**
- Modify: `src/madc_posix_io.h:43-44`, `src/madc_posix_io.cpp:196-202` (`host_path_generic`)
- Modify: `src/cli_options.cpp`
- Modify: `tests/unit/test_cli_options.cpp`

**Interfaces:**
- Consumes: Task 1's `row::type`, `row::choices`, `rf_required`, `table::groups` and `group_rule`.
- Produces:
  - `std::string madc::detail::host_path_generic(const std::string &)`.
  - In the reader (internal): `check_value(const row &, occurrence &, const std::string &what, result &)`, which Task 4 reuses, plus validation inside `parse()`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_cli_options.cpp`:

```cpp
namespace {

enum typed_id { y_jobs = 1, y_scale, y_debug, y_path, y_colour, y_a, y_b, y_need, y_req };

const char *const colour_choices[] = { "auto", "always", "never", NULL };

const cli::row typed_rows[] = {
	{ y_jobs, "jobs", { { "--jobs", value_form::equals_or_separate } }, "<n>", 0, "jobs",
	  NULL, 0, 0, cli::value_type::integer },
	{ y_scale, "scale", { { "--scale", value_form::equals } }, "<x>", 0, "scale",
	  NULL, 0, 0, cli::value_type::number },
	{ y_debug, "debug", { { "--debug", value_form::equals } }, "<yes|no>", 0, "debug",
	  NULL, 0, 0, cli::value_type::boolean },
	{ y_path, "path", { { "--path", value_form::equals } }, "<dir>", 0, "path",
	  NULL, 0, 0, cli::value_type::path },
	{ y_colour, "colour", { { "--colour", value_form::optional_equals } }, "<when>", 0, "colour",
	  NULL, 0, 0, cli::value_type::text, colour_choices },
	{ y_a, "a", { { "-a", flag } }, NULL, 0, "a" },
	{ y_b, "b", { { "-b", flag } }, NULL, 0, "b" },
	{ y_need, "need", { { "--need", flag } }, NULL, 0, "needs -a" },
	{ y_req, "req", { { "--req", value_form::equals } }, "<v>", 0, "required",
	  NULL, cli::rf_required },
};

const cli::group typed_groups[] = {
	{ cli::group_rule::exclusive, 2, { y_a, y_b } },
	{ cli::group_rule::requires_rest, 2, { y_need, y_a } },
};

cli::table typed_table()
{
	cli::table t = cli::table();
	t.program = "prog";
	t.sections = sections;
	t.nsections = 1;
	t.rows = typed_rows;
	t.nrows = sizeof(typed_rows) / sizeof(typed_rows[0]);
	t.groups = typed_groups;
	t.ngroups = sizeof(typed_groups) / sizeof(typed_groups[0]);
	t.operands = cli::operand_rule::permute;
	return t;
}

} // namespace

TEST_CASE("typed values, choices, and a bare optional value")
{
	const cli::table t = typed_table();
	const cli::result r = cli::parse(t, words({ "--req=1", "--jobs", "4", "--scale=2.5",
						    "--debug=YES", "--colour=never", "--colour" }));
	REQUIRE(r.ok());
	CHECK(r.integer(y_jobs, 0) == 4);
	CHECK(r.number(y_scale, 0) == doctest::Approx(2.5));
	CHECK(r.boolean(y_debug, false));
	CHECK(r.values(y_colour) == words({ "never", "" }));
}

TEST_CASE("a bad value names the option and what it needs")
{
	const cli::table t = typed_table();
	cli::result r = cli::parse(t, words({ "--req=1", "--jobs=x" }));
	CHECK(r.error == cli::error_kind::bad_value);
	CHECK(r.message == "invalid value 'x' for '--jobs': expected an integer");
	r = cli::parse(t, words({ "--req=1", "--jobs=" }));
	CHECK(r.message == "invalid value '' for '--jobs': expected an integer");
	r = cli::parse(t, words({ "--req=1", "--scale=1.5x" }));
	CHECK(r.message == "invalid value '1.5x' for '--scale': expected a number");
	r = cli::parse(t, words({ "--req=1", "--debug=maybe" }));
	CHECK(r.message == "invalid value 'maybe' for '--debug': expected yes or no");
	r = cli::parse(t, words({ "--req=1", "--colour=purple" }));
	CHECK(r.message == "invalid value 'purple' for '--colour': expected one of auto, always, never");
}

TEST_CASE("groups and required options")
{
	const cli::table t = typed_table();
	cli::result r = cli::parse(t, words({ "--req=1", "-a", "-b" }));
	CHECK(r.error == cli::error_kind::conflict);
	CHECK(r.message == "'-b' cannot be used with '-a'");
	r = cli::parse(t, words({ "--req=1", "--need" }));
	CHECK(r.error == cli::error_kind::missing_required);
	CHECK(r.message == "'--need' requires '-a'");
	r = cli::parse(t, words({ "--req=1", "--need", "-a" }));
	CHECK(r.ok());
	r = cli::parse(t, words({ "-a" }));
	CHECK(r.error == cli::error_kind::missing_required);
	CHECK(r.message == "the option '--req' is required");
}

TEST_CASE("a path value is stored in generic form on this host")
{
	const cli::table t = typed_table();
	const cli::result r = cli::parse(t, words({ "--req=1", "--path=a\\b/c" }));
	REQUIRE(r.ok());
#ifdef _WIN32
	CHECK(r.last(y_path)->value == "a/b/c");
#else
	CHECK(r.last(y_path)->value == "a\\b/c");	// '\' is a file-name byte here
#endif
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `bash scripts/remote_build.sh sync build`, restore `hello.cpp`, then `ssh -p 2299 dev@localhost 'cd /workspace/madc && make -C src ../bin/test_cli_options && bin/test_cli_options'`.
Expected: the bad-value and group cases FAIL, because every parse is `ok()`. The path case passes on Linux.

- [ ] **Step 3: Implement `host_path_generic`**

Search first (Rule #4). Concept: a path converted to forward slashes, "generic form". Run: `git grep -n -E 'generic|forward.?slash|replace.*\\\\' -- src/madc_posix_io.cpp src/madc_posix_io.h`. Expected: nothing. The host path family has splitters (`host_path_dirname`, `host_path_basename`) and one separator set, `host_path_separators`, but no normalizer. Put it in that family, over that set.

In `src/madc_posix_io.h`, after `std::string host_path_basename(const std::string &path);`, add:

```cpp

// `path` in generic form: every separator of the splitters' set written '/'
// (boost::filesystem's generic format). On Windows a '\' becomes '/'; on
// POSIX '\' is an ordinary file-name byte and the path comes back as it is.
// What a path-typed command-line value is stored as (cli_options.h).
std::string host_path_generic(const std::string &path);
```

In `src/madc_posix_io.cpp`, after the body of `host_path_basename`, add:

```cpp

std::string host_path_generic(const std::string &path)
{
	std::string out(path);
	for ( size_t i = 0; i < out.size(); i++ )
		if ( host_path_is_separator(out[i]) )
			out[i] = '/';
	return out;
}
```

- [ ] **Step 4: Implement validation in the reader**

In `src/cli_options.cpp`, add `#include "madc_posix_io.h"	// host_path_generic` after `#include "libmadc/cli_options.h"`.

Inside the anonymous namespace, directly after `split_response_text`, add:

```cpp
bool whole_integer(const std::string &s)
{
	if ( s.empty() )
		return false;
	errno = 0;
	char *end = NULL;
	strtoll(s.c_str(), &end, 10);
	return errno == 0 && end && *end == '\0';
}

bool whole_number(const std::string &s)
{
	if ( s.empty() )
		return false;
	errno = 0;
	char *end = NULL;
	strtod(s.c_str(), &end);
	return errno == 0 && end && *end == '\0';
}

// A command-line occurrence carries a value to check: never a flag, nor the
// bare word of a form whose value may be absent.
bool carries_value(value_form f, const std::string &value)
{
	if ( f == value_form::none )
		return false;
	return !value.empty() || (f != value_form::prefix && f != value_form::optional_equals);
}

// The value a row needs: its type, then one of its choices. A path is stored
// in generic form. `what` names where the value came from.
bool check_value(const row &rw, occurrence &o, const std::string &what, result &r)
{
	const char *expected = NULL;
	bool b;
	switch ( rw.type )
	{
	case value_type::text:
		break;
	case value_type::integer:
		if ( !whole_integer(o.value) )
			expected = "an integer";
		break;
	case value_type::number:
		if ( !whole_number(o.value) )
			expected = "a number";
		break;
	case value_type::boolean:
		if ( !parse_boolean(o.value, b) )
			expected = "yes or no";
		break;
	case value_type::path:
		o.value = madc::detail::host_path_generic(o.value);
		break;
	}
	if ( expected )
		return fail(r, error_kind::bad_value, "invalid value " + quoted(o.value)
			    + " for " + what + ": expected " + expected);
	if ( !rw.choices )
		return true;
	std::string list;
	for ( const char *const *c = rw.choices; *c; c++ )
	{
		if ( o.value == *c )
			return true;
		list += (list.empty() ? "" : ", ") + std::string(*c);
	}
	return fail(r, error_kind::bad_value, "invalid value " + quoted(o.value)
		    + " for " + what + ": expected one of " + list);
}

bool given_on_command_line(const result &r, int id)
{
	for ( size_t k = 0; k < r.options.size(); k++ )
		if ( r.options[k].id == id && !r.options[k].negated
		     && r.options[k].source == value_source::command_line )
			return true;
	return false;
}

// How a message names `id`: the spelling the command line used, else the
// row's primary spelling.
std::string spelled_name(const table &t, const result &r, int id)
{
	for ( size_t k = 0; k < r.options.size(); k++ )
		if ( r.options[k].id == id && r.options[k].spelled )
			return quoted(r.options[k].spelled);
	const row *rw = find_row(t, id);
	return quoted(rw && rw->spellings[0].text ? rw->spellings[0].text : "?");
}

bool check_groups(const table &t, result &r)
{
	for ( size_t g = 0; g < t.ngroups; g++ )
	{
		const group &gr = t.groups[g];
		if ( gr.rule == group_rule::exclusive )
		{
			const int *first = NULL;
			for ( size_t k = 0; k < gr.nids; k++ )
			{
				if ( !given_on_command_line(r, gr.ids[k]) )
					continue;
				if ( first )
					return fail(r, error_kind::conflict, spelled_name(t, r, gr.ids[k])
						    + " cannot be used with " + spelled_name(t, r, *first));
				first = &gr.ids[k];
			}
		}
		else if ( gr.nids > 0 && r.has(gr.ids[0]) )
			for ( size_t k = 1; k < gr.nids; k++ )
				if ( !r.has(gr.ids[k]) )
					return fail(r, error_kind::missing_required, spelled_name(t, r, gr.ids[0])
						    + " requires " + spelled_name(t, r, gr.ids[k]));
	}
	return true;
}

bool check_required(const table &t, result &r)
{
	for ( size_t k = 0; k < t.nrows; k++ )
	{
		const row &rw = t.rows[k];
		if ( (rw.flags & rf_required) && !(rw.flags & rf_doc) && !r.last(rw.id) )
			return fail(r, error_kind::missing_required,
				    "the option " + quoted(rw.spellings[0].text) + " is required");
	}
	return true;
}
```

`spelled_name` calls `find_row`, which the header declares, so no forward declaration is needed.

In `reader::read_option`, replace

```cpp
	if ( !take_value(m, word, i, o) )
		return false;
	_r.options.push_back(o);
```

with

```cpp
	if ( !take_value(m, word, i, o) )
		return false;
	if ( carries_value(m.spelled->form, o.value)
	     && !check_value(*m.option, o, quoted(o.spelled), _r) )
		return false;
	_r.options.push_back(o);
```

In `reader::read_bundle`, replace

```cpp
		else if ( o.value.empty() && sp->form == value_form::joined )
			return fail(_r, error_kind::missing_value, "missing argument to " + quoted(letter));
		got.push_back(o);
		break;
```

with

```cpp
		else if ( o.value.empty() && sp->form == value_form::joined )
			return fail(_r, error_kind::missing_value, "missing argument to " + quoted(letter));
		if ( carries_value(sp->form, o.value) && !check_value(*owner, o, quoted(letter), _r) )
			return false;
		got.push_back(o);
		break;
```

Replace `parse`:

```cpp
result parse(const table &t, const std::vector<std::string> &args)
{
	result r;
	if ( reader(t, args, r).run() && check_groups(t, r) )
		check_required(t, r);
	return r;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: the Step 2 commands.
Expected: `[doctest] Status: SUCCESS!`

- [ ] **Step 6: Tier 1 + Tier 2, gates, commit**

Run: `TESTS='testcli_* testinc*' bash scripts/remote_build.sh sync build fix`. `madc_posix_io.cpp` is shared by the include chain, so its neighbours ride Tier 1. Then the static gate loop, including `check-one-path-absolute.sh` and `check-one-path-within.sh`. Expected: green.
Stage `src/madc_posix_io.h src/madc_posix_io.cpp src/cli_options.cpp tests/unit/test_cli_options.cpp` and dispatch `commit-writer`. Pointers: `git diff --cached`; the unit test; the Step 3 search and its empty result.

---

### Task 4: Sources — environment, config, defaults

**Files:**
- Modify: `include/libmadc/cli_options.h` (`sources`, the `parse` signature)
- Modify: `src/cli_options.cpp`
- Modify: `tests/unit/test_cli_options.cpp`

**Interfaces:**
- Consumes: Task 3's `check_value`.
- Produces: `struct madc::cli::sources { const char *(*environment)(const char *); const std::map<madc::dis::istring, std::string> *config; bool skip_defaults; }` and `result parse(const table &, const std::vector<std::string> &, const sources &from = sources())`. The default (all zero) means no environment, no config, and defaults applied.

- [ ] **Step 1: Write the failing tests**

Add `#include <map>` to the includes of `tests/unit/test_cli_options.cpp`. Append:

```cpp
namespace {

enum source_id { s_level = 1, s_name, s_fast };

const cli::row source_rows[] = {
	{ s_level, "level", { { "--level", value_form::equals } }, "<n>", 0, "level",
	  NULL, 0, 0, cli::value_type::integer, NULL, "1", "PROG_LEVEL", "level" },
	{ s_name, "name", { { "--name", value_form::equals } }, "<s>", 0, "name",
	  NULL, 0, 0, cli::value_type::text, NULL, NULL, NULL, "name" },
	{ s_fast, "fast", { { "--fast", flag } }, NULL, 0, "fast",
	  NULL, cli::rf_negatable, 0, cli::value_type::text, NULL, NULL, "PROG_FAST" },
};

std::map<std::string, std::string> fake_env;

const char *fake_getenv(const char *name)
{
	std::map<std::string, std::string>::const_iterator it = fake_env.find(name);
	return it == fake_env.end() ? NULL : it->second.c_str();
}

cli::table source_table()
{
	cli::table t = cli::table();
	t.program = "prog";
	t.sections = sections;
	t.nsections = 1;
	t.rows = source_rows;
	t.nrows = sizeof(source_rows) / sizeof(source_rows[0]);
	t.operands = cli::operand_rule::permute;
	return t;
}

} // namespace

TEST_CASE("sources: command line > environment > config > default")
{
	const cli::table t = source_table();
	std::map<madc::dis::istring, std::string> config;
	cli::sources from = cli::sources();
	from.environment = fake_getenv;
	from.config = &config;
	fake_env.clear();
	const std::vector<std::string> none;

	cli::result r = cli::parse(t, none, from);
	REQUIRE(r.ok());
	REQUIRE(r.last(s_level) != NULL);
	CHECK(r.last(s_level)->source == cli::value_source::default_value);
	CHECK(r.integer(s_level, 0) == 1);

	config[madc::dis::istring("level")] = "2";
	r = cli::parse(t, none, from);
	CHECK(r.last(s_level)->source == cli::value_source::config);
	CHECK(r.integer(s_level, 0) == 2);

	fake_env["PROG_LEVEL"] = "3";
	r = cli::parse(t, none, from);
	CHECK(r.last(s_level)->source == cli::value_source::environment);
	CHECK(r.integer(s_level, 0) == 3);

	r = cli::parse(t, words({ "--level=5" }), from);
	CHECK(r.count(s_level) == 1);
	CHECK(r.last(s_level)->source == cli::value_source::command_line);
	CHECK(r.integer(s_level, 0) == 5);

	from.skip_defaults = true;
	fake_env.clear();
	config.clear();
	r = cli::parse(t, none, from);
	CHECK(r.last(s_level) == NULL);

	r = cli::parse(t, none);		// no sources named: defaults only
	CHECK(r.integer(s_level, 0) == 1);
}

TEST_CASE("sources: a flag reads yes or no; a bad value names its source")
{
	const cli::table t = source_table();
	std::map<madc::dis::istring, std::string> config;
	config[madc::dis::istring("name")] = "bob";
	cli::sources from = cli::sources();
	from.environment = fake_getenv;
	from.config = &config;
	const std::vector<std::string> none;

	fake_env.clear();
	fake_env["PROG_FAST"] = "yes";
	cli::result r = cli::parse(t, none, from);
	REQUIRE(r.ok());
	CHECK(r.has(s_fast));
	CHECK(r.values(s_name) == words({ "bob" }));

	fake_env["PROG_FAST"] = "no";
	r = cli::parse(t, none, from);
	REQUIRE(r.ok());
	REQUIRE(r.last(s_fast) != NULL);
	CHECK(r.last(s_fast)->negated);
	CHECK_FALSE(r.has(s_fast));

	fake_env["PROG_FAST"] = "maybe";
	r = cli::parse(t, none, from);
	CHECK(r.error == cli::error_kind::bad_value);
	CHECK(r.message == "invalid value 'maybe' for 'PROG_FAST': expected yes or no");

	fake_env.clear();
	fake_env["PROG_LEVEL"] = "x";
	r = cli::parse(t, none, from);
	CHECK(r.message == "invalid value 'x' for 'PROG_LEVEL': expected an integer");
	fake_env.clear();
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `bash scripts/remote_build.sh sync build`, restore `hello.cpp`, then `ssh -p 2299 dev@localhost 'cd /workspace/madc && make -C src ../bin/test_cli_options'`.
Expected: FAIL to compile with `'sources' is not a member of 'madc::cli'`.

- [ ] **Step 3: Implement**

In `include/libmadc/cli_options.h`, replace

```cpp
// THE reader.
result parse(const table &t, const std::vector<std::string> &args);
```

with

```cpp
// The sources beside the command line, below it in precedence: the
// environment, then the program's config file, then each row's default.
// Value-initialized (`sources()`): no environment, no config, defaults on.
struct sources
{
	const char *(*environment)(const char *name);	// getenv-shaped; NULL: none
	const std::map<madc::dis::istring, std::string> *config;	// key -> value; NULL: none
	bool skip_defaults;				// true: rows' defaults are not applied
};

// THE reader.
result parse(const table &t, const std::vector<std::string> &args,
	     const sources &from = sources());
```

In `src/cli_options.cpp`, inside the anonymous namespace after `check_required`, add:

```cpp
bool takes_value(const row &rw)
{
	for ( size_t s = 0; s < max_spellings && rw.spellings[s].text; s++ )
		if ( rw.spellings[s].form != value_form::none )
			return true;
	return false;
}

// What the sources below the command line give a row it left unset — the
// environment, then config, then the row's default — with that source and a
// name for messages; NULL when none does.
const char *source_value(const row &rw, const sources &from, value_source &src,
			 std::string &what)
{
	if ( from.environment && rw.env )
	{
		const char *v = from.environment(rw.env);
		if ( v && *v )
		{
			src = value_source::environment;
			what = quoted(rw.env);
			return v;
		}
	}
	if ( from.config && rw.config_key )
	{
		std::map<madc::dis::istring, std::string>::const_iterator it =
			from.config->find(madc::dis::istring(rw.config_key));
		if ( it != from.config->end() )
		{
			src = value_source::config;
			what = quoted(rw.config_key);
			return it->second.c_str();
		}
	}
	if ( !from.skip_defaults && rw.default_value )
	{
		src = value_source::default_value;
		what = "the default of " + quoted(rw.spellings[0].text ? rw.spellings[0].text : rw.name);
		return rw.default_value;
	}
	return NULL;
}

// Every row the command line left unset takes its value from the sources;
// a flag's value from them is a yes or a no.
bool apply_sources(const table &t, const sources &from, result &r)
{
	for ( size_t k = 0; k < t.nrows; k++ )
	{
		const row &rw = t.rows[k];
		if ( (rw.flags & rf_doc) || r.last(rw.id) )
			continue;
		occurrence o;
		o.id = rw.id;
		o.spelled = NULL;
		o.negated = false;
		std::string what;
		const char *v = source_value(rw, from, o.source, what);
		if ( !v )
			continue;
		o.value = v;
		if ( takes_value(rw) )
		{
			if ( !check_value(rw, o, what, r) )
				return false;
			r.options.push_back(o);
			continue;
		}
		bool on;
		if ( !parse_boolean(o.value, on) )
			return fail(r, error_kind::bad_value, "invalid value " + quoted(o.value)
				    + " for " + what + ": expected yes or no");
		o.value.clear();
		o.negated = !on;
		if ( on || (rw.flags & rf_negatable) )
			r.options.push_back(o);
	}
	return true;
}
```

Replace `parse`:

```cpp
result parse(const table &t, const std::vector<std::string> &args, const sources &from)
{
	result r;
	if ( reader(t, args, r).run() && apply_sources(t, from, r) && check_groups(t, r) )
		check_required(t, r);
	return r;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `ssh -p 2299 dev@localhost 'cd /workspace/madc && make -C src ../bin/test_cli_options && bin/test_cli_options'` after a sync, build and `hello.cpp` restore.
Expected: `[doctest] Status: SUCCESS!`

- [ ] **Step 5: Tier 1 + Tier 2, gates, commit**

Run: `TESTS='testcli_*' bash scripts/remote_build.sh sync build fix`, then the static gate loop, including `check-istring-fields.sh`. If its count for the two engine files grew, re-record it as in Task 1 Step 5; the growth is built text: messages and values. Expected: green.
Stage the three files (plus `scripts/istring_keep.tsv` if it was re-recorded) and dispatch `commit-writer`. Pointers: `git diff --cached`; the unit test; spec section 4.3.

---

### Task 5: Writers — help, `--help=json`, completion scripts; the one width rule

**Files:**
- Modify: `include/libmadc/cli_options.h` (the writer declarations)
- Modify: `src/cli_options.cpp`
- Modify: `include/madc_console.h`, `src/madc_console.cpp` (`terminal_columns`)
- Modify: `src/lexer.cpp:11823-11824` (the diagnostic echo reads `terminal_columns`)
- Modify: `tests/unit/test_cli_options.cpp`

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces:
  - `void madc::cli::write_help(std::ostream &, const table &, size_t width)`
  - `std::string madc::cli::describe_json(const table &)`
  - `enum class madc::cli::shell { bash, zsh, fish, powershell }`
  - `bool madc::cli::parse_shell(const std::string &, shell &)`
  - `void madc::cli::write_completion(std::ostream &, const table &, shell)`
  - `size_t madc::terminal_columns()`

- [ ] **Step 1: Find the existing width rule (Rule #4)**

Concept: the terminal width that laid-out text wraps to. Run: `git grep -n -E 'COLUMNS|TIOCGWINSZ' -- src include/*.h`. Expected:
- `src/lexer.cpp:11823`, the diagnostic source echo: `getenv("COLUMNS")`, else 80.
- `src/ui_term.cpp:333`, the TUI's own window size, a different concern: a full-screen grid, not laid-out text.

The help needs the first rule, so that rule moves to the console owner (`madc_console.h`: "the process's standard streams and the terminal they reach"), and both read it from there.

- [ ] **Step 2: Write the failing tests**

Add `#include <cstdlib>`, `#include "json.hpp"` and `#include "madc_console.h"` to the includes of `tests/unit/test_cli_options.cpp`. Append:

```cpp
namespace {

const cli::section help_sections[] = {
	{ "Input", "What to read." },
	{ "Output", NULL },
};

const cli::row help_rows[] = {
	{ 0, NULL, {}, NULL, 0, "the program to run", "<file>", cli::rf_doc },
	{ 1, "verbose", { { "-v", flag }, { "--verbose", flag } }, NULL, 0, "say more" },
	{ 2, "output", { { "-o", value_form::separate } }, "<file>", 1,
	  "write the result to <file> instead of the terminal, which is where it goes otherwise" },
	{ 3, "std", { { "--std", value_form::equals } }, "<std>", 1, "the language standard",
	  NULL, 0, 0, cli::value_type::text, NULL, "c17", "PROG_STD", "std" },
	{ 4, "secret", { { "--secret", flag } }, NULL, 1, "hidden", NULL, cli::rf_hidden },
	{ 5, "long", { { "--a-very-long-option-name", value_form::equals } }, "<value>", 1,
	  "long left column" },
};

cli::table help_table()
{
	cli::table t = cli::table();
	t.program = "prog";
	t.usage = "[options] <file>";
	t.description = "A test program.";
	t.sections = help_sections;
	t.nsections = 2;
	t.rows = help_rows;
	t.nrows = sizeof(help_rows) / sizeof(help_rows[0]);
	t.operands = cli::operand_rule::permute;
	return t;
}

// One help line: the left column padded to the help column (26).
std::string help_line(const std::string &left, const std::string &help)
{
	std::string s = left;
	s.resize(26, ' ');
	return s + help + "\n";
}

} // namespace

TEST_CASE("help: sections, notes, wrapping, a long left column, hidden rows left out")
{
	std::ostringstream os;
	cli::write_help(os, help_table(), 80);
	const std::string expected =
		"Usage: prog [options] <file>\n"
		"\n"
		"A test program.\n"
		"\n"
		"Input:\n"
		"  What to read.\n"
		+ help_line("  <file>", "the program to run")
		+ help_line("  -v, --verbose", "say more")
		+ "\n"
		"Output:\n"
		+ help_line("  -o <file>", "write the result to <file> instead of the terminal,")
		+ help_line("", "which is where it goes otherwise")
		+ help_line("  --std=<std>", "the language standard [default: c17] [env: PROG_STD]")
		+ help_line("", "[config: std]")
		+ "  --a-very-long-option-name=<value>\n"
		+ help_line("", "long left column");
	CHECK(os.str() == expected);
}

TEST_CASE("--help=json describes every visible row")
{
	const nlohmann::json d = nlohmann::json::parse(cli::describe_json(help_table()));
	CHECK(d["schema"] == 1);
	CHECK(d["program"] == "prog");
	REQUIRE(d["sections"].size() == 2);
	CHECK(d["sections"][0]["title"] == "Input");
	CHECK(d["sections"][0]["entries"][0]["usage"] == "<file>");
	CHECK(d["sections"][0]["options"][0]["name"] == "verbose");
	const nlohmann::json &out = d["sections"][1]["options"];
	REQUIRE(out.size() == 3);			// --secret is hidden
	CHECK(out[0]["spellings"][0]["text"] == "-o");
	CHECK(out[0]["spellings"][0]["form"] == "separate");
	CHECK(out[1]["default"] == "c17");
	CHECK(out[1]["env"] == "PROG_STD");
	CHECK(out[1]["config_key"] == "std");
	CHECK(out[1]["type"] == "text");
}

TEST_CASE("completion scripts for four shells")
{
	const cli::table t = help_table();
	cli::shell sh;
	CHECK(cli::parse_shell("bash", sh));
	CHECK(sh == cli::shell::bash);
	CHECK_FALSE(cli::parse_shell("tcsh", sh));

	std::ostringstream bash, zsh, fish, ps;
	cli::write_completion(bash, t, cli::shell::bash);
	cli::write_completion(zsh, t, cli::shell::zsh);
	cli::write_completion(fish, t, cli::shell::fish);
	cli::write_completion(ps, t, cli::shell::powershell);

	CHECK(bash.str().find("complete -F _prog_complete prog\n") != std::string::npos);
	CHECK(bash.str().find("'-o') COMPREPLY=( $(compgen -f -- \"$cur\") )") != std::string::npos);
	CHECK(bash.str().find("compgen -W '-v --verbose -o --std= --a-very-long-option-name='")
	      != std::string::npos);
	CHECK(zsh.str().compare(0, 15, "#compdef prog\n#") == 0);
	CHECK(zsh.str().find("equals=( '--std=' '--a-very-long-option-name=' )") != std::string::npos);
	CHECK(fish.str().find("complete -c prog -s 'v' -d 'say more'\n") != std::string::npos);
	CHECK(fish.str().find("complete -c prog -l 'std' -r -d 'the language standard'\n")
	      != std::string::npos);
	CHECK(ps.str().find("Register-ArgumentCompleter -Native -CommandName prog") != std::string::npos);
	CHECK(ps.str().find("'--std='") != std::string::npos);
	const std::string all = bash.str() + zsh.str() + fish.str() + ps.str();
	CHECK(all.find("secret") == std::string::npos);
}

TEST_CASE("terminal_columns: $COLUMNS when it is a positive number, else 80")
{
	setenv("COLUMNS", "100", 1);
	CHECK(madc::terminal_columns() == 100);
	setenv("COLUMNS", "abc", 1);
	CHECK(madc::terminal_columns() == 80);
	unsetenv("COLUMNS");
	CHECK(madc::terminal_columns() == 80);
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `bash scripts/remote_build.sh sync build`, restore `hello.cpp`, then `ssh -p 2299 dev@localhost 'cd /workspace/madc && make -C src ../bin/test_cli_options'`.
Expected: FAIL to compile with `'write_help' is not a member of 'madc::cli'`.

- [ ] **Step 4: Implement the width rule**

In `include/madc_console.h`, add `#include <cstddef>` beside `#include <iosfwd>`. After the `sgr` function, add:

```cpp

// The width laid-out text wraps to (a diagnostic's source echo, the help):
// $COLUMNS when it holds a positive number, else 80. The one rule; the TUI's
// full-screen grid asks the terminal itself (ui_term.cpp).
size_t terminal_columns();
```

In `src/madc_console.cpp`, the first `namespace madc { ... }` block (lines 85–120) sits outside every `#ifdef`. Before its closing `} // namespace madc`, add the following (`<stdlib.h>` is already included):

```cpp

size_t terminal_columns()
{
    const char *env = getenv("COLUMNS");
    const long n = env ? atol(env) : 0;
    return n > 0 ? (size_t)n : 80;
}
```

In `src/lexer.cpp:11823-11824`, replace

```cpp
    char *env_columns = getenv("COLUMNS");
    size_t term_columns = env_columns ? (size_t)atoi(env_columns) : 80;
```

with

```cpp
    const size_t term_columns = madc::terminal_columns();
```

`lexer.cpp` already includes `madc_console.h` (it calls `madc::colour_reaches`); check with `git grep -n madc_console.h -- src/lexer.cpp` and add the include if it is missing.

- [ ] **Step 5: Implement the writers**

In `include/libmadc/cli_options.h`, before `} // namespace cli`, add:

```cpp
// --- The writers: the same rows as help, JSON and completion scripts ---

// The help: the usage line, the description, each section (its note, then
// its rows and help lines), the epilogue, wrapped to `width` columns (at
// least 40). Hidden rows are left out.
void write_help(std::ostream &os, const table &t, size_t width);

// The table as JSON — what `madc --help=json` prints and madcide reads:
// {schema, program, usage, description, sections: [{title, note, options:
// [{name, spellings: [{text, form}], usage, help, type, metavar, choices,
// default, env, config_key, required, negatable, ends_options}], entries:
// [{usage, help}]}]}. Hidden rows are left out.
std::string describe_json(const table &t);

enum class shell : uint8_t { bash, zsh, fish, powershell };
// A shell's name ("bash", "zsh", "fish", "powershell") as its code.
bool parse_shell(const std::string &name, shell &out);
// A completion script for `sh` from the table's visible spellings: options
// after a '-', files after a spelling whose value is the next word, files
// otherwise.
void write_completion(std::ostream &os, const table &t, shell sh);
```

In `src/cli_options.cpp`, add `#include "json.hpp"` after `#include "madc_posix_io.h"`. Inside the anonymous namespace, after `apply_sources`, add:

```cpp
const size_t help_column = 26;

std::string metavar_of(const row &rw)
{
	return rw.metavar ? rw.metavar : "<value>";
}

std::string spelling_display(const row &rw, const spelling &sp)
{
	std::string s(sp.text);
	switch ( sp.form )
	{
	case value_form::none:
		break;
	case value_form::separate:
		s += " " + metavar_of(rw);
		break;
	case value_form::joined:
	case value_form::joined_or_separate:
		s += metavar_of(rw);
		break;
	case value_form::prefix:
		if ( rw.metavar )
			s += "[" + metavar_of(rw) + "]";
		break;
	case value_form::equals:
	case value_form::equals_or_separate:
		s += "=" + metavar_of(rw);
		break;
	case value_form::optional_equals:
		s += "[=" + metavar_of(rw) + "]";
		break;
	}
	return s;
}

// Help's left column for a row: its usage, else its spellings.
std::string row_display(const row &rw)
{
	if ( rw.usage )
		return rw.usage;
	std::string s;
	for ( size_t k = 0; k < max_spellings && rw.spellings[k].text; k++ )
	{
		if ( k )
			s += ", ";
		s += spelling_display(rw, rw.spellings[k]);
	}
	if ( rw.flags & rf_negatable )
		for ( size_t k = 0; k < max_spellings && rw.spellings[k].text; k++ )
			if ( rw.spellings[k].form == value_form::none
			     && starts_with(rw.spellings[k].text, "--") )
				s += ", --no-" + std::string(rw.spellings[k].text + 2);
	return s;
}

// A row's help with what the reader knows of it: [default: v] [env: X]
// [config: k].
std::string help_text(const row &rw)
{
	std::string s = rw.help ? rw.help : "";
	if ( rw.default_value )
		s += std::string(s.empty() ? "" : " ") + "[default: " + rw.default_value + "]";
	if ( rw.env )
		s += std::string(s.empty() ? "" : " ") + "[env: " + rw.env + "]";
	if ( rw.config_key )
		s += std::string(s.empty() ? "" : " ") + "[config: " + rw.config_key + "]";
	return s;
}

// `text` as lines of at most `room` columns: each '\n'-separated paragraph
// on its own, a paragraph's leading blanks kept on each of its lines, a word
// wider than the room alone on its line.
std::vector<std::string> wrap(const std::string &text, size_t room)
{
	std::vector<std::string> lines;
	size_t start = 0;
	for ( ;; )
	{
		const size_t nl = text.find('\n', start);
		const std::string para = text.substr(start, nl == std::string::npos
						     ? std::string::npos : nl - start);
		const size_t lead = para.find_first_not_of(' ');
		if ( lead == std::string::npos )
			lines.push_back(std::string());
		else
		{
			const std::string indent = para.substr(0, lead);
			std::istringstream in(para);
			std::string word, line = indent;
			while ( in >> word )
			{
				if ( line.size() > indent.size() && line.size() + 1 + word.size() > room )
				{
					lines.push_back(line);
					line = indent;
				}
				if ( line.size() > indent.size() )
					line += ' ';
				line += word;
			}
			lines.push_back(line);
		}
		if ( nl == std::string::npos )
			return lines;
		start = nl + 1;
	}
}

// One output line, its trailing blanks dropped.
void put_line(std::ostream &os, std::string line)
{
	const size_t end = line.find_last_not_of(' ');
	line.erase(end == std::string::npos ? 0 : end + 1);
	os << line << "\n";
}

void put_wrapped(std::ostream &os, const std::string &text, size_t width,
		 const std::string &indent)
{
	const std::vector<std::string> lines = wrap(text, width - indent.size());
	for ( size_t l = 0; l < lines.size(); l++ )
		put_line(os, indent + lines[l]);
}

const char *form_name(value_form f)
{
	switch ( f )
	{
	case value_form::none:			return "none";
	case value_form::separate:		return "separate";
	case value_form::joined:		return "joined";
	case value_form::prefix:		return "prefix";
	case value_form::joined_or_separate:	return "joined_or_separate";
	case value_form::equals:		return "equals";
	case value_form::optional_equals:	return "optional_equals";
	case value_form::equals_or_separate:	return "equals_or_separate";
	}
	return "none";
}

const char *type_name(value_type v)
{
	switch ( v )
	{
	case value_type::text:		return "text";
	case value_type::integer:	return "integer";
	case value_type::number:	return "number";
	case value_type::boolean:	return "boolean";
	case value_type::path:		return "path";
	}
	return "text";
}

// The words a completion offers for options: each visible spelling, a form
// whose value follows '=' with its '='.
std::vector<std::string> completion_words(const table &t)
{
	std::vector<std::string> out;
	each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
		if ( rw.flags & rf_hidden )
			return false;
		std::string w(sp.text);
		if ( sp.form == value_form::equals || sp.form == value_form::equals_or_separate )
			w += '=';
		out.push_back(w);
		return false;
	});
	return out;
}

// The visible spellings whose bare word takes the next word as its value.
std::vector<std::string> value_next_words(const table &t)
{
	std::vector<std::string> out;
	each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
		if ( !(rw.flags & rf_hidden) && value_follows(sp.form) )
			out.push_back(sp.text);
		return false;
	});
	return out;
}

// `s` single-quoted for a POSIX shell (bash, zsh): a ' becomes '\''.
std::string sh_quote(const std::string &s)
{
	std::string out = "'";
	for ( size_t i = 0; i < s.size(); i++ )
		out += s[i] == '\'' ? std::string("'\\''") : std::string(1, s[i]);
	return out + "'";
}

// `s` single-quoted for fish: ' and \ escaped with a backslash.
std::string fish_quote(const std::string &s)
{
	std::string out = "'";
	for ( size_t i = 0; i < s.size(); i++ )
	{
		if ( s[i] == '\'' || s[i] == '\\' )
			out += '\\';
		out += s[i];
	}
	return out + "'";
}

// `s` single-quoted for PowerShell: a ' doubled.
std::string ps_quote(const std::string &s)
{
	std::string out = "'";
	for ( size_t i = 0; i < s.size(); i++ )
		out += s[i] == '\'' ? std::string("''") : std::string(1, s[i]);
	return out + "'";
}

// A word of bash's compgen -W list: glob characters escaped (compgen expands
// each word and removes the escapes).
std::string compgen_word(const std::string &s)
{
	std::string out;
	for ( size_t i = 0; i < s.size(); i++ )
	{
		if ( strchr("?*[]\\", s[i]) )
			out += '\\';
		out += s[i];
	}
	return out;
}

// A shell identifier from the program's name.
std::string ident(const char *program)
{
	std::string out;
	for ( const char *p = program ? program : ""; *p; p++ )
		out += isalnum((unsigned char)*p) ? *p : '_';
	return out;
}

// A row's help, short enough for a completion menu: its first paragraph,
// cut at a word before 60 columns.
std::string short_help(const row &rw)
{
	std::string s = rw.help ? rw.help : "";
	const size_t nl = s.find('\n');
	if ( nl != std::string::npos )
		s.erase(nl);
	if ( s.size() <= 60 )
		return s;
	size_t cut = s.rfind(' ', 57);
	if ( cut == std::string::npos )
		cut = 57;
	return s.substr(0, cut) + "...";
}

void write_bash(std::ostream &os, const table &t)
{
	const std::string fn = "_" + ident(t.program) + "_complete";
	os << "# bash completion for " << t.program << " - generated by `"
	   << t.program << " --completion=bash`\n";
	os << fn << "()\n{\n";
	os << "\tlocal cur=\"${COMP_WORDS[COMP_CWORD]}\"\n";
	os << "\tlocal prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n";
	const std::vector<std::string> after = value_next_words(t);
	if ( !after.empty() )
	{
		os << "\tcase \"$prev\" in\n\t\t";
		for ( size_t k = 0; k < after.size(); k++ )
			os << (k ? "|" : "") << sh_quote(after[k]);
		os << ") COMPREPLY=( $(compgen -f -- \"$cur\") ); return 0 ;;\n\tesac\n";
	}
	std::string list;
	const std::vector<std::string> all = completion_words(t);
	for ( size_t k = 0; k < all.size(); k++ )
		list += (k ? " " : "") + compgen_word(all[k]);
	os << "\tif [[ \"$cur\" == -* ]]; then\n";
	os << "\t\tCOMPREPLY=( $(compgen -W " << sh_quote(list) << " -- \"$cur\") )\n";
	os << "\t\t[[ \"${COMPREPLY[0]}\" == *= ]] && compopt -o nospace\n";
	os << "\t\treturn 0\n\tfi\n";
	os << "\tCOMPREPLY=( $(compgen -f -- \"$cur\") )\n}\n";
	os << "complete -F " << fn << " " << t.program << "\n";
}

void write_zsh(std::ostream &os, const table &t)
{
	const std::string fn = "_" + ident(t.program);
	std::string plain, equals, after;
	const std::vector<std::string> all = completion_words(t);
	for ( size_t k = 0; k < all.size(); k++ )
		(all[k][all[k].size() - 1] == '=' ? equals : plain) += " " + sh_quote(all[k]);
	const std::vector<std::string> next = value_next_words(t);
	for ( size_t k = 0; k < next.size(); k++ )
		after += " " + sh_quote(next[k]);
	os << "#compdef " << t.program << "\n";
	os << "# zsh completion for " << t.program << " - generated by `"
	   << t.program << " --completion=zsh`\n";
	os << fn << "() {\n";
	os << "\tlocal -a plain equals after\n";
	os << "\tplain=(" << plain << " )\n";
	os << "\tequals=(" << equals << " )\n";
	os << "\tafter=(" << after << " )\n";
	os << "\tif (( ${after[(Ie)${words[CURRENT-1]}]} )); then\n\t\t_files\n";
	os << "\telif [[ $PREFIX == -* ]]; then\n";
	os << "\t\tcompadd -- $plain\n\t\tcompadd -S '' -- $equals\n";
	os << "\telse\n\t\t_files\n\tfi\n}\n";
	os << fn << " \"$@\"\n";
}

void write_fish(std::ostream &os, const table &t)
{
	os << "# fish completion for " << t.program << " - generated by `"
	   << t.program << " --completion=fish`\n";
	each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
		if ( rw.flags & rf_hidden )
			return false;
		const std::string text(sp.text);
		os << "complete -c " << t.program;
		if ( text.compare(0, 2, "--") == 0 )
			os << " -l " << fish_quote(text.substr(2));
		else if ( text.size() == 2 )
			os << " -s " << fish_quote(text.substr(1));
		else
			os << " -o " << fish_quote(text.substr(1));
		if ( sp.form != value_form::none )
		{
			if ( rw.choices )
			{
				std::string list;
				for ( const char *const *c = rw.choices; *c; c++ )
					list += (list.empty() ? "" : " ") + std::string(*c);
				os << " -x -a " << fish_quote(list);
			}
			else if ( value_follows(sp.form) )
				os << " -r -F";
			else
				os << " -r";
		}
		const std::string h = short_help(rw);
		if ( !h.empty() )
			os << " -d " << fish_quote(h);
		os << "\n";
		return false;
	});
}

void write_powershell(std::ostream &os, const table &t)
{
	os << "# PowerShell completion for " << t.program << " - generated by `"
	   << t.program << " --completion=powershell`\n";
	os << "Register-ArgumentCompleter -Native -CommandName " << t.program
	   << " -ScriptBlock {\n";
	os << "\tparam($wordToComplete, $commandAst, $cursorPosition)\n";
	os << "\tif (-not \"$wordToComplete\".StartsWith('-')) { return }\n";
	os << "\t$options = @(";
	const std::vector<std::string> all = completion_words(t);
	for ( size_t k = 0; k < all.size(); k++ )
		os << (k ? ", " : "") << ps_quote(all[k]);
	os << ")\n";
	os << "\t$options | Where-Object { $_.StartsWith(\"$wordToComplete\") } | ForEach-Object {\n";
	os << "\t\t[System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterName', $_)\n";
	os << "\t}\n}\n";
}
```

After `find_row` (outside the anonymous namespace), add:

```cpp
void write_help(std::ostream &os, const table &t, size_t width)
{
	if ( width < 40 )
		width = 40;
	os << "Usage: " << (t.program ? t.program : "") << " "
	   << (t.usage ? t.usage : "[options]") << "\n";
	if ( t.description )
	{
		os << "\n";
		put_wrapped(os, t.description, width, "");
	}
	for ( size_t s = 0; s < t.nsections; s++ )
	{
		bool any = t.sections[s].note != NULL;
		for ( size_t k = 0; k < t.nrows && !any; k++ )
			any = t.rows[k].section == s && !(t.rows[k].flags & rf_hidden);
		if ( !any )
			continue;
		os << "\n" << t.sections[s].title << ":\n";
		if ( t.sections[s].note )
			put_wrapped(os, t.sections[s].note, width, "  ");
		for ( size_t k = 0; k < t.nrows; k++ )
		{
			const row &rw = t.rows[k];
			if ( rw.section != s || (rw.flags & rf_hidden) )
				continue;
			std::string left = "  " + row_display(rw);
			const std::string help = help_text(rw);
			if ( left.size() + 2 > help_column )
			{
				put_line(os, left);
				left.clear();
				if ( help.empty() )
					continue;
			}
			const std::vector<std::string> lines = wrap(help, width - help_column);
			for ( size_t l = 0; l < lines.size(); l++ )
			{
				std::string col = l == 0 ? left : std::string();
				col.resize(help_column, ' ');
				put_line(os, col + lines[l]);
			}
		}
	}
	if ( t.epilog )
	{
		os << "\n";
		put_wrapped(os, t.epilog, width, "");
	}
}

std::string describe_json(const table &t)
{
	nlohmann::json d;
	d["schema"] = 1;
	d["program"] = t.program ? t.program : "";
	d["usage"] = t.usage ? t.usage : "";
	if ( t.description )
		d["description"] = t.description;
	d["sections"] = nlohmann::json::array();
	for ( size_t s = 0; s < t.nsections; s++ )
	{
		nlohmann::json js;
		js["title"] = t.sections[s].title ? t.sections[s].title : "";
		if ( t.sections[s].note )
			js["note"] = t.sections[s].note;
		js["options"] = nlohmann::json::array();
		js["entries"] = nlohmann::json::array();
		for ( size_t k = 0; k < t.nrows; k++ )
		{
			const row &rw = t.rows[k];
			if ( rw.section != s || (rw.flags & rf_hidden) )
				continue;
			if ( rw.flags & rf_doc )
			{
				nlohmann::json e;
				e["usage"] = row_display(rw);
				e["help"] = rw.help ? rw.help : "";
				js["entries"].push_back(e);
				continue;
			}
			nlohmann::json o;
			o["name"] = rw.name ? rw.name : "";
			o["spellings"] = nlohmann::json::array();
			for ( size_t p = 0; p < max_spellings && rw.spellings[p].text; p++ )
			{
				nlohmann::json sp;
				sp["text"] = rw.spellings[p].text;
				sp["form"] = form_name(rw.spellings[p].form);
				o["spellings"].push_back(sp);
			}
			o["usage"] = row_display(rw);
			o["help"] = rw.help ? rw.help : "";
			o["type"] = type_name(rw.type);
			if ( rw.metavar )
				o["metavar"] = rw.metavar;
			if ( rw.choices )
			{
				o["choices"] = nlohmann::json::array();
				for ( const char *const *c = rw.choices; *c; c++ )
					o["choices"].push_back(*c);
			}
			if ( rw.default_value )
				o["default"] = rw.default_value;
			if ( rw.env )
				o["env"] = rw.env;
			if ( rw.config_key )
				o["config_key"] = rw.config_key;
			o["required"] = (rw.flags & rf_required) != 0;
			o["negatable"] = (rw.flags & rf_negatable) != 0;
			o["ends_options"] = (rw.flags & rf_ends_options) != 0;
			js["options"].push_back(o);
		}
		d["sections"].push_back(js);
	}
	return d.dump(2);
}

bool parse_shell(const std::string &name, shell &out)
{
	if ( name == "bash" )
		out = shell::bash;
	else if ( name == "zsh" )
		out = shell::zsh;
	else if ( name == "fish" )
		out = shell::fish;
	else if ( name == "powershell" )
		out = shell::powershell;
	else
		return false;
	return true;
}

void write_completion(std::ostream &os, const table &t, shell sh)
{
	switch ( sh )
	{
	case shell::bash:
		write_bash(os, t);
		return;
	case shell::zsh:
		write_zsh(os, t);
		return;
	case shell::fish:
		write_fish(os, t);
		return;
	case shell::powershell:
		write_powershell(os, t);
		return;
	}
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `bash scripts/remote_build.sh sync build`, restore `hello.cpp`, then `ssh -p 2299 dev@localhost 'cd /workspace/madc && make -C src ../bin/test_cli_options && bin/test_cli_options'`.
Expected: `[doctest] Status: SUCCESS!` If the help case fails, compare `os.str()` against `expected` line by line. The layout rules are column 26, a left column wider than 24 on its own line, and greedy wrapping. Fix the writer, not the expectation, unless the expectation breaks one of those rules.

- [ ] **Step 7: Tier 1 + Tier 2, gates, commit**

Run: `TESTS='testcli_* testdiag*' bash scripts/remote_build.sh sync build fix`. The diagnostic echo now reads `terminal_columns`, so the caret tests (`testdiag*`) ride Tier 1. Then run the static gate loop and `ssh -p 2299 dev@localhost 'cd /workspace/madc && make -C src ../bin/test_diag_caret && bin/test_diag_caret'`. Expected: green.
Stage `include/libmadc/cli_options.h src/cli_options.cpp include/madc_console.h src/madc_console.cpp src/lexer.cpp tests/unit/test_cli_options.cpp` and dispatch `commit-writer`. Pointers: `git diff --cached`; the unit tests; the Step 1 search result (one `COLUMNS` reader, moved to its owner).

---
### Task 6: madc's table and its lane rule

**Files:**
- Create: `include/madc_cli.h`
- Create: `src/madc_cli.cpp`
- Create: `tests/unit/test_madc_cli.cpp`
- Modify: `src/Makefile:135` (`madc_cli.o` after `cli_options.o`)
- Modify: `scripts/check-istring-fields.sh` (`CORE`), `scripts/istring_keep.tsv`

**Interfaces:**
- Consumes: Tasks 1–5 (`madc::cli::*`); `madc::file_kind_of_path` and `madc::file_kind_family` (`include/madc_file_kinds.h`).
- Produces:
  - `enum class cli_option : int` (every value below);
  - `enum madc_cli_tag : uint32_t { mt_stage, mt_introspection }`;
  - `const madc::cli::table &madc_cli_table()`.
  Task 7 switches on `cli_option`, and `madc_capabilities.cpp` reads `mt_introspection`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_madc_cli.cpp`:

```cpp
// Unit tests for madc's own command-line table (src/madc_cli.cpp) over the
// option reader: the table's invariants and madc's lane rule. Oracle for the
// lane rule: what main()'s ladder answered before the table
// (tests/testcli_compiler_line.mad, tests/testcli_preprocess_output.mad),
// widened only where the plan says (docs/plans/2026-10-16-cli-options-plan.md,
// "Deliberate behaviour changes").

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <initializer_list>
#include <set>
#include <string>
#include <vector>

#include "madc_cli.h"

namespace {

namespace cli = madc::cli;

std::vector<std::string> words(std::initializer_list<const char *> w)
{
	return std::vector<std::string>(w.begin(), w.end());
}

cli::result parse(std::initializer_list<const char *> w)
{
	return cli::parse(madc_cli_table(), words(w));
}

int id(cli_option o)
{
	return (int)o;
}

} // namespace

TEST_CASE("every spelling is one row's; every option has a name and help")
{
	const cli::table &t = madc_cli_table();
	std::set<std::string> spellings, names;
	for ( size_t k = 0; k < t.nrows; k++ )
	{
		const cli::row &rw = t.rows[k];
		CHECK(rw.help != NULL);
		if ( rw.flags & cli::rf_doc )
		{
			CHECK(rw.usage != NULL);
			continue;
		}
		REQUIRE(rw.name != NULL);
		CHECK_MESSAGE(names.insert(rw.name).second, rw.name);
		CHECK(rw.spellings[0].text != NULL);
		for ( size_t s = 0; s < cli::max_spellings && rw.spellings[s].text; s++ )
			CHECK_MESSAGE(spellings.insert(rw.spellings[s].text).second, rw.spellings[s].text);
	}
}

TEST_CASE("the stage options: gcc's, plus madc's aliases")
{
	const cli::table &t = madc_cli_table();
	std::set<std::string> stage;
	for ( size_t k = 0; k < t.nrows; k++ )
		if ( t.rows[k].tags & mt_stage )
			for ( size_t s = 0; s < cli::max_spellings && t.rows[k].spellings[s].text; s++ )
				stage.insert(t.rows[k].spellings[s].text);
	const std::set<std::string> want = { "-c", "-shared", "-r", "-o", "-E", "-dM",
					     "--emit-object", "--emit-executable", "--dump-source" };
	CHECK(stage == want);
}

TEST_CASE("the introspection list --capabilities prints, in its order")
{
	const cli::table &t = madc_cli_table();
	std::vector<std::string> got;
	for ( size_t k = 0; k < t.nrows; k++ )
		if ( t.rows[k].tags & mt_introspection )
			got.push_back(std::string(t.rows[k].spellings[0].text).substr(2));
	CHECK(got == words({ "dump-source", "dump-cir", "dump-nodes", "dump-cir-checked",
			     "dump-forest", "dump-registered", "show-stats" }));
}

TEST_CASE("a script keeps its argv; a line that builds permutes")
{
	cli::result r = parse({ "x.mad", "-o", "out", "@args", "--", "-v" });
	REQUIRE(r.ok());
	CHECK(r.options.empty());
	CHECK(r.remainder == words({ "x.mad", "-o", "out", "@args", "--", "-v" }));

	r = parse({ "-O2", "x.c", "-o", "x", "-lm" });
	REQUIRE(r.ok());
	CHECK(r.operands == words({ "x.c" }));
	REQUIRE(r.options.size() == 3);
	CHECK(r.options[2].id == id(cli_option::library));

	r = parse({ "x.c", "--emit-object", "x.o" });		// widened: madc's alias is a stage
	REQUIRE(r.ok());
	CHECK(r.operands == words({ "x.c" }));
	CHECK(r.has(id(cli_option::emit_object)));

	r = parse({ "-c", "x.c", "-fPIC", "-o", "x.o" });
	REQUIRE(r.ok());
	CHECK(r.operands == words({ "x.c" }));

	r = parse({ "-E", "-o", "x.i", "x.c" });
	REQUIRE(r.ok());
	CHECK(r.operands == words({ "x.c" }));

	r = parse({ "-", "x" });
	REQUIRE(r.ok());
	CHECK(r.remainder == words({ "-", "x" }));
}

TEST_CASE("-o beside --emit-pch names the .madh; the script keeps its argv")
{
	const cli::result r = parse({ "--emit-pch", "-o", "out.madh", "x.mad", "-v" });
	REQUIRE(r.ok());
	CHECK(r.operands.empty());
	CHECK(r.remainder == words({ "x.mad", "-v" }));
}

TEST_CASE("madc's spellings: gcc's -std=, warning selection, refusals by row, ends-options")
{
	cli::result r = parse({ "-std=c17", "-Wall", "-Wno-unused", "-Werror=x", "-Wl,-rpath,/x",
				"-Wp,-MD,x.d", "-O", "-dM" });
	REQUIRE(r.ok());
	REQUIRE(r.options.size() == 8);
	CHECK(r.options[0].id == id(cli_option::language_std));
	CHECK(r.options[0].value == "c17");
	CHECK(r.options[1].id == id(cli_option::warning_selection));
	CHECK(r.options[2].id == id(cli_option::warning_selection));
	CHECK(r.options[3].id == id(cli_option::warnings_as_errors));
	CHECK(r.options[4].id == id(cli_option::linker_option));
	CHECK(r.options[4].value == "-rpath,/x");
	CHECK(r.options[5].id == id(cli_option::pass_through));
	CHECK(r.options[6].id == id(cli_option::opt_level));
	CHECK(r.options[7].id == id(cli_option::dump_macros));

	r = parse({ "--forest-relayout=img", "a.log", "-v" });
	REQUIRE(r.ok());
	CHECK(r.remainder == words({ "a.log", "-v" }));

	r = parse({ "-stdlib=libc++", "--std=c++17" });
	REQUIRE(r.ok());
	CHECK(r.options[0].id == id(cli_option::stdlib_flavor));
	CHECK(r.options[1].id == id(cli_option::language_std));
}

TEST_CASE("madc's errors")
{
	cli::result r = parse({ "-fbogus" });
	CHECK(r.message == "unrecognized command-line option '-fbogus'");
	r = parse({ "--dump-sorce", "x.c" });
	CHECK(r.message == "unrecognized command-line option '--dump-sorce'; did you mean '--dump-source'?");
	r = parse({ "-o" });
	CHECK(r.message == "missing argument to '-o'");
	r = parse({ "-O4" });
	CHECK(r.message == "invalid value '4' for '-O': expected one of 0, 1, 2, 3");
	r = parse({ "--help=xml" });
	CHECK(r.message == "invalid value 'xml' for '--help': expected one of json");
	r = parse({ "--completion=tcsh" });
	CHECK(r.message == "invalid value 'tcsh' for '--completion': expected one of bash, zsh, fish, powershell");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bash scripts/remote_build.sh sync build`, restore `hello.cpp`, then `ssh -p 2299 dev@localhost 'cd /workspace/madc && make -C src ../bin/test_madc_cli'`.
Expected: FAIL to compile, with `madc_cli.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/madc_cli.h`:

```cpp
#ifndef __MADC_CLI_H
#define __MADC_CLI_H 1

// madc_cli.h — madc's OWN command line as one table over the option reader
// (libmadc/cli_options.h): main() parses with it and switches on cli_option;
// the help, --help=json, the completion scripts and the --capabilities
// introspection list are written from it.
//
// THREAD-SAFETY CONTRACT: the table is constant data — read-only, shared.

#include <cstdint>

#include "libmadc/cli_options.h"

// Every madc option, by code (enum-over-strings: main() switches on these,
// and -Wswitch names an option that has no case).
enum class cli_option : int
{
	none = 0,		// help lines (rf_doc rows)
	// Input / mode
	interactive, lsp, mcp, history_file, project, preprocess,
	// Language / preprocessor
	language_std, stdlib_flavor, define, include_dir, library, library_dir,
	linker_option, no_auto_load, no_includes, no_embedded_headers, no_posix_compat,
	// Codegen
	opt_level, no_builtin, instrument_functions,
	// Output (no run)
	emit, emit_pch, emit_function, dump_source, dump_cir, dump_nodes, dump_cir_checked,
	// Frozen forest
	freeze, freeze_append, freeze_mir_cache, freeze_ledger, freeze_also_std,
	freeze_sources, no_sysroot_includes, run_frozen, freeze_run, pack_forest,
	dump_forest, forest_bind, forest_touch_log, forest_relayout, no_forest_bind,
	dump_registered, dump_macros,
	// Misc
	show_stats, debug_info, no_warnings, warning_selection, warnings_as_errors,
	pass_through, diagnostics_color, no_diagnostics_color, verbose, help,
	completion, version, capabilities, dump_machine,
	// Configuration file
	config, no_config,
	// AOT output
	compile_object, relocatable, output, static_libmadc, no_eval_shims,
	eval_shims, pie, no_pie, gui_subsystem, console_subsystem, shared,
	emit_object, emit_executable, pic
};

// madc's own row tags (row::tags; the reader never reads them).
enum madc_cli_tag : uint32_t
{
	mt_stage = 1u << 0,		// a stage option: the line builds, no program
					// runs (gcc's -c -shared -r -o -E -dM; madc's aliases)
	mt_introspection = 1u << 1	// a dump the --capabilities manifest lists
};

// THE table.
const madc::cli::table &madc_cli_table();

#endif
```

- [ ] **Step 4: Write the table**

Create `src/madc_cli.cpp`. The help texts are `print_usage`'s (`src/madc.cpp:274-507`), unwrapped into paragraphs. The writer wraps them. Changes from the old text:
- The madc.ini keys gain the `modules` and `theme` lines the old help omitted (`register_madc_keys`, `src/madc_config.cpp:35-47`, is the key owner).
- New rows document `-dumpmachine`, the `-fPIC` family and `-feval-shims`.

```cpp
// src/madc_cli.cpp — madc's own command line: every option as one row of one
// table (include/madc_cli.h), and the rule for where the program's own argv
// begins. The reader is libmadc/cli_options.h; main() (src/madc.cpp) applies
// what it reads. The madc.ini key lines document register_madc_keys
// (src/madc_config.cpp), the owner of those spellings.

#include "madc_cli.h"

#include "madc_file_kinds.h"	// the compiler-line rule reads a source's family

using madc::cli::row;
using madc::cli::value_form;
using madc::cli::value_type;

namespace {

const value_form flag = value_form::none;
const value_form sep = value_form::separate;
const value_form glued = value_form::joined;
const value_form pfx = value_form::prefix;
const value_form both = value_form::joined_or_separate;
const value_form eq = value_form::equals;
const value_form opt_eq = value_form::optional_equals;

const uint32_t doc = madc::cli::rf_doc;
const uint32_t hidden = madc::cli::rf_hidden;

constexpr int id(cli_option o)
{
	return (int)o;
}

enum : unsigned
{
	s_input, s_language, s_codegen, s_output, s_forest, s_misc, s_config, s_env, s_aot
};

const madc::cli::section sections[] = {
	{ "Input / mode", NULL },
	{ "Language / preprocessor (gcc/clang-style)", NULL },
	{ "Codegen", NULL },
	{ "Output (no run)", NULL },
	{ "Frozen forest (compile once, run without parsing)", NULL },
	{ "Misc", NULL },
	{ "Configuration file (optional; CLI > environment > madc.ini > defaults)",
	  "Searched in order; the first existing file wins (files are never merged): "
	  "./madc.ini, $XDG_CONFIG_HOME/madc/madc.ini (else %APPDATA%\\madc\\madc.ini on "
	  "Windows, else ~/.config/madc/madc.ini), then the system config dir. Keys (an unknown "
	  "one is an error, not a warning) are listed below; relative paths resolve against the "
	  "config file's own directory, and ~/ works." },
	{ "Environment", NULL },
	{ "AOT output (gcc vocabulary; do not run)", NULL },
};

const char *const opt_levels[] = { "0", "1", "2", "3", NULL };
const char *const json_only[] = { "json", NULL };
const char *const shells[] = { "bash", "zsh", "fish", "powershell", NULL };

// Field order: id, name, spellings, metavar, section, help, usage, flags,
// tags, type, choices (cli_options.h).
const row rows[] = {
	// ---- Input / mode
	{ 0, NULL, {}, NULL, s_input, "compile and JIT-run a single source file", "<file>", doc },
	{ 0, NULL, {}, NULL, s_input,
	  "the REPL when stdin is a terminal; otherwise compile and run stdin, as if named -",
	  "(no file)", doc },
	{ 0, NULL, {}, NULL, s_input, "read the program from stdin (gcc, python)", "-", doc },
	{ id(cli_option::interactive), "interactive", { { "-i", flag }, { "--interactive", flag } },
	  NULL, s_input,
	  "the REPL, even when stdin is not a terminal; with a <file>, run it first and keep its "
	  "names (python -i). A line starting with : is an ex command on <file>'s buffer (vim's "
	  "colon mode: :p :s :g :w :e ...; :q quits)" },
	{ id(cli_option::lsp), "lsp", { { "--lsp", flag } }, NULL, s_input,
	  "a Language Server Protocol server on stdin/stdout (Content-Length framed): "
	  "diagnostics, symbols, definition, references, hover, semantic tokens for the "
	  "documents a client opens; <file> is parsed before the client asks",
	  "--lsp [<file>]" },
	{ id(cli_option::mcp), "mcp", { { "--mcp", flag } }, NULL, s_input,
	  "a Model Context Protocol server on stdin/stdout (one JSON-RPC message per line): the "
	  "code-graph tools over <file>, for the operator",
	  "--mcp <file>" },
	{ id(cli_option::history_file), "history_file", { { "--history-file", eq } }, "PATH|no",
	  s_input,
	  "where the REPL keeps its history (default $MADC_HISTORY_FILE, else "
	  "$XDG_STATE_HOME/madc/history); no keeps none" },
	{ id(cli_option::project), "project", { { "--project", sep } }, "<prj.json>", s_input,
	  "build from a project manifest: compile each translation unit, link the modules, run "
	  "the entry. Two shapes by top-level JSON kind: an OBJECT is the native manifest "
	  "(tus/entry/output — what madcide writes), an ARRAY is a compile_commands.json import" },
	{ 0, NULL, {}, NULL, s_input,
	  "a .json source is treated as a project manifest (implicit --project; gcc/clang-style "
	  "by extension)",
	  "<file.json>", doc },
	{ 0, NULL, {}, NULL, s_input,
	  "execute madc-compiled relocatable objects (-c/-r output) as a precompiled cache: MIR's "
	  "in-process loader maps and relocates them — no recompilation; several .o inputs merge "
	  "first (multi-object link); with -o/-shared/-r they LINK into one artifact",
	  "<file.o>...", doc },
	{ id(cli_option::preprocess), "preprocess", { { "-E", flag } }, NULL, s_input,
	  "preprocess only: print the expanded source, or write it to the file -o names",
	  NULL, 0, mt_stage },

	// ---- Language / preprocessor
	{ id(cli_option::language_std), "std", { { "--std", eq }, { "-std", eq } }, "<std>",
	  s_language,
	  "c89/c90/c99/c11/c17/c23 (c = c11), gnuNN, c++NN, gnu++NN, or madc (C++ keywords "
	  "reserved). Without it the extension decides, as gcc/g++ do: .c gnu17, "
	  ".cpp/.cc/.cxx gnu++17, .mad madc; else madc.ini's std, else madc. A --project TU alike." },
	{ id(cli_option::stdlib_flavor), "stdlib", { { "-stdlib", eq } }, "<lib>", s_language,
	  "C++ standard library flavor (clang's spelling): libstdc++ / libc++, whichever this "
	  "build could probe. REPLACES the C++ include search list, as clang does — it is not "
	  "the same as putting the library first with -I." },
	{ id(cli_option::define), "define", { { "-D", both } }, "<name>[=value]", s_language,
	  "define a preprocessor macro" },
	{ id(cli_option::include_dir), "include", { { "-I", both } }, "<dir>", s_language,
	  "add an include search directory" },
	{ id(cli_option::library), "library", { { "-l", glued } }, "<name>", s_language,
	  "bind a library so its symbols resolve at link time (e.g. -lcrypt): a module or bare "
	  "name spelled for the target by the module map (-lm = libm.so.6 / libSystem.B.dylib / "
	  "ucrtbase.dll; lib<name>.<dso> otherwise; a path verbatim). The build-line form of the "
	  "source's `import`. Works with or without --project. In AOT mode it becomes a "
	  "DT_NEEDED / load command / PE import; otherwise the library is opened." },
	{ id(cli_option::library_dir), "library_dir", { { "-L", both } }, "<dir>", s_language,
	  "search <dir> for the -l libraries ahead of the toolchain's directories (ld's -L "
	  "order): a run opens the library found, an image records it as ld does (its SONAME; "
	  "nothing for an empty archive)" },
	{ id(cli_option::linker_option), "linker_option", { { "-Wl,", glued } }, "<option>",
	  s_language,
	  "add <dir> to the produced image's run-time library path (DT_RUNPATH / LC_RPATH), "
	  "after madc's own; any other -Wl, linker option is refused",
	  "-Wl,-rpath,<dir>" },
	{ id(cli_option::no_auto_load), "no_auto_load", { { "--no-auto-load", flag } }, NULL,
	  s_language,
	  "do not act on `import` / #load library bindings (the library is linked, not loaded); "
	  "link explicitly via -l. The namespace binds to the program's own scope." },
	{ id(cli_option::no_includes), "no_includes", { { "--no-includes", flag } }, NULL,
	  s_language, "do not process #include directives" },
	{ id(cli_option::no_embedded_headers), "no_embedded_headers",
	  { { "--no-embedded-headers", flag } }, NULL, s_language,
	  "disable baked-in headers; use real system headers" },
	{ id(cli_option::no_posix_compat), "no_posix_compat", { { "--no-posix-compat", flag } },
	  NULL, s_language, "disable Win64's additive POSIX header supplements" },

	// ---- Codegen
	{ id(cli_option::opt_level), "opt_level", { { "-O", pfx } }, "<level>", s_codegen,
	  "JIT optimization level (bare -O = -O1)", "-O, -O0 .. -O3", 0, 0, value_type::text,
	  opt_levels },
	{ id(cli_option::no_builtin), "no_builtin", { { "-fno-builtin-", glued } }, "<name>",
	  s_codegen, "disable a specific builtin" },
	{ id(cli_option::instrument_functions), "instrument_functions",
	  { { "--finstrument-functions", flag } }, NULL, s_codegen,
	  "emit __cyg_profile instrumentation hooks" },

	// ---- Output (no run)
	{ id(cli_option::emit), "emit", { { "--emit", eq } }, "c11|mc11", s_output,
	  "render the cir_node tree as C / MC11 source" },
	{ id(cli_option::emit_pch), "emit_pch", { { "--emit-pch", flag } }, NULL, s_output,
	  "write a .madh precompiled header (-o names it)" },
	{ id(cli_option::emit_function), "emit_function", { { "--emit-function", sep } }, "<name>",
	  s_output, "print one function's source" },
	{ id(cli_option::dump_source), "dump_source", { { "--dump-source", flag } }, NULL, s_output,
	  "reconstruct full-fidelity source", NULL, 0, mt_stage | mt_introspection },
	{ id(cli_option::dump_cir), "dump_cir", { { "--dump-cir", flag } }, NULL, s_output,
	  "dump the cir_node tree before checking", NULL, 0, mt_introspection },
	{ id(cli_option::dump_nodes), "dump_nodes", { { "--dump-nodes", flag } }, NULL, s_output,
	  "dump the cir_node tree through madc's walker", NULL, 0, mt_introspection },
	{ id(cli_option::dump_cir_checked), "dump_cir_checked", { { "--dump-cir-checked", flag } },
	  NULL, s_output, "dump the cir_node tree after c2mir's check (the c2m -d stage)",
	  NULL, 0, mt_introspection },

	// ---- Frozen forest
	{ id(cli_option::freeze), "freeze", { { "--freeze", eq } }, "<file>", s_forest,
	  "parse + translate, then freeze the module tree into a forest snapshot container (no run)" },
	{ id(cli_option::freeze_append), "freeze_append", { { "--freeze-append", eq } }, "<bin>",
	  s_forest,
	  "same, but append the container to an existing binary (found later from its EOF footer)" },
	{ id(cli_option::freeze_mir_cache), "freeze_mir_cache", { { "--freeze-mir-cache", flag } },
	  NULL, s_forest,
	  "also compile the frozen module and pack its MIR binary form as a cache segment "
	  "(consumers skip node rebuild + c2mir; absent = normal fallback)" },
	{ id(cli_option::freeze_ledger), "freeze_ledger", { { "--freeze-ledger", eq } }, "<src>",
	  s_forest,
	  "also compile this C-lane runtime source into the container's AOT ledger segment, so "
	  "-static-libmadc programs carry it (repeatable; the build's list is "
	  "scripts/ledger_sources.txt)" },
	{ id(cli_option::freeze_also_std), "freeze_also_std", { { "--freeze-also-std", eq } },
	  "<std>", s_forest,
	  "label the frozen profile to serve compiles under this --std= too (repeatable; the "
	  "release pack names the C++ modes that share madc's corpus)" },
	{ id(cli_option::freeze_sources), "freeze_sources", { { "--freeze-sources", eq } },
	  "<list>", s_forest,
	  "also pack the exact text of every file <list> names (one path per line) as "
	  "source-only units, served to a compile with no headers on disk (repeatable; the "
	  "release pack lists every system header)" },
	{ id(cli_option::no_sysroot_includes), "no_sysroot_includes",
	  { { "--no-sysroot-includes", flag } }, NULL, s_forest,
	  "resolve <...> only from the toolchain's own roots (the C++ stdlib dirs + the "
	  "compiler's builtin dir) and the embedded headers — never the host's C library / SDK "
	  "dirs. The freezer's posture: a frozen forest is the header-less consumer's corpus" },
	{ id(cli_option::run_frozen), "run_frozen", { { "--run-frozen", opt_eq } }, "<file>",
	  s_forest,
	  "thaw + compile + run a frozen container; with no value, load the blob appended to "
	  "this executable. Remaining arguments become the program's argv" },
	{ id(cli_option::freeze_run), "freeze_run", { { "--freeze-run", flag } }, NULL, s_forest,
	  "freeze to a temp container, then re-exec this madc in a FRESH process to run it "
	  "(round-trip)" },
	{ id(cli_option::pack_forest), "pack_forest", { { "--pack-forest", eq } }, "<file>",
	  s_forest,
	  "with -o/-shared: the emitted native image also carries this frozen container in its "
	  "self-image carrier (ELF: appended trailer; Mach-O: a __MADC,__forest section signed "
	  "at emit)" },
	{ id(cli_option::dump_forest), "dump_forest", { { "--dump-forest", opt_eq } }, "<file>",
	  s_forest,
	  "print a container's directory + grove payloads (decl index, PP exports, edges, branch "
	  "macros, canonical order); no value = this executable's blob",
	  NULL, 0, mt_introspection },
	{ id(cli_option::forest_bind), "forest_bind", { { "--forest-bind", opt_eq } }, "<file>",
	  s_forest,
	  "bind grove-backed system #includes from a frozen container instead of live-parsing; "
	  "no value = the discovery chain below. This is the DEFAULT for compiles (live "
	  "fall-through when nothing is found); freeze modes live-parse unless it is passed "
	  "explicitly.\n"
	  "Discovery order (first usable container wins):\n"
	  "1. this binary's own image (ELF trailer / Mach-O __MADC,__forest section),\n"
	  "2. the libmadc image, when linked shared,\n"
	  "3. <exe>.forest, then <lib>.forest (sidecars),\n"
	  "4. the $MADC_FOREST path,\n"
	  "5. the madc.ini `forest' key" },
	{ id(cli_option::forest_touch_log), "forest_touch_log", { { "--forest-touch-log", eq } },
	  "<file>", s_forest,
	  "(pack step) record the frozen-table reads this compile's bind makes, for "
	  "--forest-relayout" },
	{ id(cli_option::forest_relayout), "forest_relayout", { { "--forest-relayout", eq } },
	  "<image>", s_forest,
	  "(pack step) lay the image's last forest container out by the recorded reads; every "
	  "argument after it is a log",
	  "--forest-relayout=<image> <log>...", madc::cli::rf_ends_options },
	{ id(cli_option::no_forest_bind), "no_forest_bind", { { "--no-forest-bind", flag } }, NULL,
	  s_forest,
	  "force live parse (overrides the default and an explicit --forest-bind; the A/B "
	  "measurement lever)" },
	{ id(cli_option::dump_registered), "dump_registered", { { "--dump-registered", flag } },
	  NULL, s_forest,
	  "parse, then print the registered name maps (forest index-parity oracle input; no run)",
	  NULL, 0, mt_introspection },
	{ id(cli_option::dump_macros), "dump_macros", { { "-dM", flag } }, NULL, s_forest,
	  "preprocess, then print the effective macro table (gcc -dM -E analogue; no parse, no run)",
	  NULL, 0, mt_stage },

	// ---- Misc
	{ id(cli_option::show_stats), "show_stats", { { "--show-stats", flag } }, NULL, s_misc,
	  "print input/token/timing stats (read, lex, parse, c2mir, execute) to stderr after the run",
	  NULL, 0, mt_introspection },
	{ id(cli_option::debug_info), "debug_info", { { "-g", flag } }, NULL, s_misc,
	  "debug info: gdb can break/step/inspect the JIT'd program (forces -O0, no inlining, "
	  "spill-all)" },
	{ id(cli_option::no_warnings), "no_warnings", { { "-w", flag } }, NULL, s_misc,
	  "inhibit all warning messages (gcc -w)" },
	{ id(cli_option::warning_selection), "warning_selection", { { "-W", pfx } }, "<name>",
	  s_misc,
	  "gcc's warning selection (-Wall, -Wextra, ...): accepted; madc reports one fixed set "
	  "(-Werror is refused)",
	  "-W<name>, -Wno-<name>" },
	{ id(cli_option::warnings_as_errors), "warnings_as_errors", { { "-Werror", pfx } }, NULL,
	  s_misc, "refused: madc's warnings are never errors", NULL, hidden },
	{ id(cli_option::pass_through), "pass_through", { { "-Wa,", glued }, { "-Wp,", glued } },
	  "<option>", s_misc,
	  "refused: madc has no separate assembler or preprocessor to pass it to", NULL, hidden },
	{ id(cli_option::diagnostics_color), "diagnostics_color",
	  { { "-fdiagnostics-color", opt_eq } }, "WHEN", s_misc,
	  "colour diagnostics (gcc's spelling): WHEN is auto (the default: a terminal only, never "
	  "with NO_COLOR set), always, or never; bare = always" },
	{ id(cli_option::no_diagnostics_color), "no_diagnostics_color",
	  { { "-fno-diagnostics-color", flag } }, NULL, s_misc, "never colour diagnostics" },
	{ id(cli_option::verbose), "verbose", { { "-v", flag }, { "--verbose", flag } }, NULL,
	  s_misc, "verbose / debug output" },
	{ id(cli_option::help), "help", { { "-h", flag }, { "-?", flag }, { "--help", opt_eq } },
	  "json", s_misc, "show this help; --help=json describes every option as JSON",
	  "-h, -?, --help[=json]", 0, 0, value_type::text, json_only },
	{ id(cli_option::completion), "completion", { { "--completion", eq } }, "<shell>", s_misc,
	  "print a completion script for bash, zsh, fish or powershell and exit",
	  NULL, 0, 0, value_type::text, shells },
	{ id(cli_option::version), "version", { { "-V", flag }, { "--version", flag } }, NULL,
	  s_misc,
	  "print the madc version (and the cross target, if this artifact has one) and exit" },
	{ id(cli_option::capabilities), "capabilities", { { "--capabilities", opt_eq } }, "json",
	  s_misc,
	  "print the build's machine-readable capability manifest (no source file required) "
	  "and exit",
	  "--capabilities=json" },
	{ id(cli_option::dump_machine), "dump_machine", { { "-dumpmachine", flag } }, NULL, s_misc,
	  "print the target triple the toolchain names (gcc -dumpmachine) and exit" },

	// ---- Configuration file
	{ id(cli_option::config), "config", { { "--config", eq } }, "<file>", s_config,
	  "read this madc.ini instead of searching; a file that fails to load is an error, "
	  "never ignored" },
	{ id(cli_option::no_config), "no_config", { { "--no-config", flag } }, NULL, s_config,
	  "skip the search entirely (hermetic builds)" },
	{ 0, NULL, {}, NULL, s_config, "default dialect (a --std= on the CLI wins)",
	  "std = c17", doc },
	{ 0, NULL, {}, NULL, s_config, "default C++ stdlib flavor (a -stdlib= on the CLI wins)",
	  "stdlib = libc++", doc },
	{ 0, NULL, {}, NULL, s_config, "frozen forest container (discovery arm 5)",
	  "forest = <file>", doc },
	{ 0, NULL, {}, NULL, s_config, "extra include dir, repeatable, searched after -I",
	  "include = <dir>", doc },
	{ 0, NULL, {}, NULL, s_config, "a module to load, as -l<name> (repeatable)",
	  "modules = <name>", doc },
	{ 0, NULL, {}, NULL, s_config,
	  "the REPL's colours: a .theme file in place of the built-in theme",
	  "theme = <file>", doc },
	{ 0, NULL, {}, NULL, s_config, "MADC_CPU_LIMIT default (auto = off)",
	  "cpu-limit = off|auto|<secs>", doc },
	{ 0, NULL, {}, NULL, s_config, "MADC_MEM_LIMIT default",
	  "mem-limit = off|auto|<MB>", doc },

	// ---- Environment
	{ 0, NULL, {}, NULL, s_env,
	  "RLIMIT_CPU guard (default off; auto is off too — madc also runs the program, so no "
	  "finite default is safe; for embedding hosts and sandboxes)",
	  "MADC_CPU_LIMIT=off|auto|<secs>", doc },
	{ 0, NULL, {}, NULL, s_env,
	  "address-space guard (RLIMIT_AS, a SOFT limit covering JIT mappings); default off; "
	  "auto = 4096 MB + 128 MB per --project TU (the test runner asks for auto). A program "
	  "that imports a GUI module lifts it at run start. Trips name the knob.",
	  "MADC_MEM_LIMIT=off|auto|<MB>", doc },
	{ 0, NULL, {}, NULL, s_env,
	  "frozen forest container to bind when no earlier discovery arm (binary image, libmadc "
	  "image, <exe>.forest / <lib>.forest sidecars) carries one; beats the madc.ini `forest' "
	  "key, which is last",
	  "MADC_FOREST=<file>", doc },
	{ 0, NULL, {}, NULL, s_env,
	  "where the madc.ini search looks for the user config (<dir>/madc/madc.ini; default "
	  "%APPDATA% on Windows, ~/.config elsewhere)",
	  "XDG_CONFIG_HOME=<dir>", doc },

	// ---- AOT output
	{ id(cli_option::compile_object), "compile_object", { { "-c", flag } }, NULL, s_aot,
	  "compile to a relocatable native object (default: <source-base>.o in the current dir; "
	  "--project: one .o per TU, gcc semantics)",
	  "-c [-o file.o]", 0, mt_stage },
	{ id(cli_option::relocatable), "relocatable", { { "-r", flag } }, NULL, s_aot,
	  "relocatable link output (gcc/ld -r): ONE .o carrying the whole input — with --project "
	  "the whole-program capture (default name: a.out)",
	  "-r [-o file.o]", 0, mt_stage },
	{ id(cli_option::output), "output", { { "-o", sep } }, "<file>", s_aot,
	  "compile to a native executable — MIR assembles the ELF directly (no external "
	  "toolchain); needs libmadc.so at run time (DT_RUNPATH is set); position-independent "
	  "(PIE) by default, gcc parity. With -c / -r / -shared it names that artifact; with "
	  "-E / -dM, the preprocessed text's file; with --emit-pch, the .madh",
	  NULL, 0, mt_stage },
	{ id(cli_option::static_libmadc), "static_libmadc",
	  { { "-static-libmadc", flag }, { "-static", flag } }, NULL, s_aot,
	  "carry the madc runtime pieces the program needs INSIDE the emitted image (from this "
	  "binary's AOT ledger), so it runs with no madc library at all; libc/libstdc++ stay "
	  "dynamic. Covers the C lane (try/catch, VLA scope exit); a program needing the C++ "
	  "script-lane runtime refuses, naming the symbols. -static is an alias (gcc "
	  "-static-libgcc precedent)" },
	{ id(cli_option::no_eval_shims), "no_eval_shims", { { "-fno-eval-shims", flag } }, NULL,
	  s_aot,
	  "leave the __madc_shim_* host-call adapters out of the artifact: it will not be called "
	  "through the value ABI by a libmadc host. An executable emit implies this; pass it when "
	  "compiling .o files you will link with -static-libmadc (the adapters need value-ABI "
	  "accessors, which are not on the ledger)" },
	{ id(cli_option::eval_shims), "eval_shims", { { "-feval-shims", flag } }, NULL, s_aot,
	  "keep the host-call adapters (undoes -fno-eval-shims)" },
	{ id(cli_option::pie), "pie", { { "-pie", flag } }, NULL, s_aot,
	  "keep the PIE layout (the default)" },
	{ id(cli_option::no_pie), "no_pie", { { "-no-pie", flag } }, NULL, s_aot,
	  "drop the PIE layout: a fixed-base ET_EXEC instead of the ET_DYN PIE" },
	{ id(cli_option::gui_subsystem), "gui_subsystem", { { "-mwindows", flag } }, NULL, s_aot,
	  "Windows: a windowed program (no console at start); a --project build reads the "
	  "manifest's \"kind\" (console | gui) instead" },
	{ id(cli_option::console_subsystem), "console_subsystem", { { "-mconsole", flag } }, NULL,
	  s_aot, "Windows: the console subsystem (the default)" },
	{ id(cli_option::shared), "shared", { { "-shared", flag } }, NULL, s_aot,
	  "compile to a shared object (ET_DYN, MIR-assembled; dlopen/import-consumable; PIC, no "
	  "TEXTREL)",
	  "-shared [-o file.so]", 0, mt_stage },
	{ id(cli_option::emit_object), "emit_object", { { "--emit-object", sep } }, "<path>", s_aot,
	  "an alias of -c -o <path>", NULL, 0, mt_stage },
	{ id(cli_option::emit_executable), "emit_executable", { { "--emit-executable", sep } },
	  "<path>", s_aot, "an alias of -o <path>", NULL, 0, mt_stage },
	{ id(cli_option::pic), "pic",
	  { { "-fPIC", flag }, { "-fpic", flag }, { "-fPIE", flag }, { "-fpie", flag } }, NULL,
	  s_aot,
	  "accepted: madc's code is position-independent already (a -c object links into a gcc "
	  "-shared library with no text relocations; an executable is PIE unless -no-pie)" },
};

// A C or C++ source (by extension) with a stage option AFTER it is a compiler
// line, permuted as gcc's (configure's `cc -O2 x.c -o x -lm`). A madc script
// keeps its argv: `madc tool.mad -o out` runs tool.mad (owner, 2026-10-16).
bool compiler_line_source(const madc::cli::table &t, const std::vector<std::string> &words,
			  size_t at)
{
	const int64_t family = madc::file_kind_family(madc::file_kind_of_path(words[at].c_str()));
	if ( family != madc::fkC && family != madc::fkCPP )
		return false;
	for ( size_t j = at + 1; j < words.size(); j++ )
	{
		const madc::cli::match m = madc::cli::match_word(t, words[j]);
		if ( m.option && (m.option->tags & mt_stage) )
			return true;
	}
	return false;
}

// Whether madc's options continue past the operand words[at]. A line that
// builds runs no program, so nothing after its source is a script's argv:
// options and operands interleave as gcc's do (`-c x.c -fPIC -o x.o`;
// ccache's `-E -o x.i x.c`). That lane is on once a stage option was read
// (-o beside --emit-pch names the .madh, not an artifact), once the lane took
// an operand, or for a compiler line. Otherwise this operand is the program
// and the rest of the line its argv.
bool madc_operand_continues(const madc::cli::table &t, const madc::cli::result &so_far,
			    const std::vector<std::string> &words, size_t at)
{
	if ( !so_far.operands.empty() )
		return true;
	const bool pch = so_far.has(id(cli_option::emit_pch));
	for ( size_t k = 0; k < so_far.options.size(); k++ )
	{
		const madc::cli::occurrence &o = so_far.options[k];
		const row *rw = madc::cli::find_row(t, o.id);
		if ( rw && (rw->tags & mt_stage) && !(pch && o.id == id(cli_option::output)) )
			return true;
	}
	return compiler_line_source(t, words, at);
}

const madc::cli::table madc_table = {
	"madc",
	"[options] <source> [program-args...]",
	"madc — My Advanced Dialect of C. Parses a C/C++ dialect into a cir_node tree, lowers "
	"it through c2mir -> MIR, and JIT-executes it (or emits C / a PCH).",
	NULL,
	sections, sizeof(sections) / sizeof(sections[0]),
	rows, sizeof(rows) / sizeof(rows[0]),
	NULL, 0,
	false,				// gcc: no bundling (-dM is a word, not -d -M)
	true,				// @file response files (gcc)
	madc::cli::operand_rule::caller,
	madc_operand_continues,
	NULL
};

} // namespace

const madc::cli::table &madc_cli_table()
{
	return madc_table;
}
```

Add `madc_cli.o` to `CORE_OFILES` in `src/Makefile:135`, directly after `cli_options.o`. Append `src/madc_cli.cpp include/madc_cli.h` to the `CORE` list in `scripts/check-istring-fields.sh`.

- [ ] **Step 5: Run the test to verify it passes**

Run: `bash scripts/remote_build.sh sync build`, restore `hello.cpp`, then `ssh -p 2299 dev@localhost 'cd /workspace/madc && make -C src ../bin/test_madc_cli && bin/test_madc_cli'`.
Expected: `[doctest] Status: SUCCESS!` Record the istring baseline for the two new files as in Task 1 Step 5. They hold no `std::string` fields, only the policy's parameters.

- [ ] **Step 6: Tier 1 + Tier 2, gates, commit**

Run: `TESTS='testcli_*' bash scripts/remote_build.sh sync build fix`, then the static gate loop. Expected: green. `main()` does not read the table yet.
Stage `include/madc_cli.h src/madc_cli.cpp tests/unit/test_madc_cli.cpp src/Makefile scripts/check-istring-fields.sh scripts/istring_keep.tsv` and dispatch `commit-writer`. Pointers: `git diff --cached`; the unit test; `print_usage` at `src/madc.cpp:274-507` as the source of every help text; spec section 5.

---

### Task 7: madc's `main()` on the reader

**Files:**
- Modify: `src/madc.cpp`:
  - delete `print_usage` (lines 272–508);
  - delete `warning_selection_option`, `stage_option` and `compiler_line_source` (lines 560–597);
  - replace the option variables and loop (lines 943–1459);
  - update the help block (1503–1507), the config step's stdlib call (1560) and `repl_options` (2006–2008).
- Modify: `src/madc_capabilities.cpp:20-24, 99-103` (introspection from the table)
- Modify: `include/madc.h:7486`, `src/lexer.cpp:5699-5708`, `src/madc_cir.cpp:8011` (`set_stdlib_flavor`)
- Create: `tests/testcli_option_engine.mad`, `.expect`, `.exe_skip`, `.win64_skip`

**Interfaces:**
- Consumes: `madc_cli_table()`, `cli_option`, `mt_introspection` (Task 6); `madc::cli::parse`, `write_help`, `describe_json`, `write_completion`, `parse_shell` (Tasks 1–5); `madc::terminal_columns()` (Task 5).
- Produces: `bool Program::set_stdlib_flavor(const madc::dis::istring &name)`, which replaces `set_stdlib_flavor_option`. madc's user-visible command line changes as listed in Global Constraints, "Deliberate behaviour changes".

- [ ] **Step 1: Write the failing integration test**

Create `tests/testcli_option_engine.mad`:

```c
// madc's command line read by the one option reader (src/madc_cli.cpp over
// include/libmadc/cli_options.h): an unknown option is an error in gcc's
// words with a "did you mean"; a missing value is an error; -Wp, is refused
// by name; gcc's -std= spelling; `--` ends madc's options; @file expands
// among madc's options but never in a script's argv; madc's --emit-object
// alias makes a compiler line; --help, --help=json and --completion=bash
// come from the table.
// Before: -fbogus, a trailing -o and -Wp,... were each opened as the source
// file; -std=c17 was not an option; `--` and @file were source names;
// `x.c --emit-object x.o` RAN x.c with two arguments.
// Oracle: gcc 13 -- `gcc -fbogus`: "unrecognized command-line option
// '-fbogus'" (rc 1); `gcc @rsp ans.c` with rsp "-DANSWER=42" prints 42;
// `gcc -std=c17` gives __STDC_VERSION__ 201710; `gcc -- -dash.c` reads the
// file named -dash.c.
int main()
{
	var dir = "tmp/testcli_option_engine";
	php::mkdir(dir.c_str());
	const char *cp = madc::compiler_path();
	var err = format("{}/err.txt", dir);
	var out = format("{}/out.txt", dir);
	var text;

	long rc = system(format("{} --no-config -fbogus 2> {}", cp, err));
	php::file_get_contents(text, err.c_str());
	println("unknown: rc={} {}", rc != 0 ? 1 : 0, php::trim(text));

	rc = system(format("{} --no-config --dump-sorce x.c 2> {}", cp, err));
	php::file_get_contents(text, err.c_str());
	println("near: rc={} {}", rc != 0 ? 1 : 0, php::trim(text));

	rc = system(format("{} --no-config -o 2> {}", cp, err));
	php::file_get_contents(text, err.c_str());
	println("missing: rc={} {}", rc != 0 ? 1 : 0, php::trim(text));

	rc = system(format("{} --no-config -Wp,-MD,x.d x.c 2> {}", cp, err));
	php::file_get_contents(text, err.c_str());
	println("pass-through: rc={} {}", rc != 0 ? 1 : 0, php::trim(text));

	rc = system(format("{} --no-config -fdiagnostics-color= x.c 2> {}", cp, err));
	php::file_get_contents(text, err.c_str());
	println("colour empty: rc={} {}", rc != 0 ? 1 : 0, php::trim(text));

	var std_c = format("{}/std.c", dir);
	php::file_put_contents(std_c.c_str(),
		"#include <stdio.h>\nint main(void)\n{\n\tprintf(\"gcc std: %ld\\n\", (long)__STDC_VERSION__);\n\treturn 0;\n}\n");
	system(format("{} --no-config -std=c17 {}", cp, std_c));

	var dash = format("{}/-dash.mad", dir);
	php::file_put_contents(dash.c_str(), "int main()\n{\n\tprintln(\"dashdash: ran\");\n\treturn 0;\n}\n");
	system(format("cd {} && {} --no-config -- -dash.mad", dir, cp));

	var rsp = format("{}/args.rsp", dir);
	php::file_put_contents(rsp.c_str(), "--no-config -DANSWER=42\n");
	var ans = format("{}/ans.c", dir);
	php::file_put_contents(ans.c_str(),
		"#include <stdio.h>\nint main(void)\n{\n\tprintf(\"response file: %d\\n\", ANSWER);\n\treturn 0;\n}\n");
	system(format("{} @{} {}", cp, rsp, ans));

	var tool = format("{}/tool.mad", dir);
	php::file_put_contents(tool.c_str(),
		"int main(int argc, char **argv)\n{\n\tprint(\"script argv:\");\n\tfor ( int i = 1; i < argc; i++ )\n\t\tprint(\" {}\", argv[i]);\n\tprintln(\"\");\n\treturn 0;\n}\n");
	system(format("{} --no-config {} -o out @args -- -v", cp, tool));

	var main_c = format("{}/main.c", dir);
	php::file_put_contents(main_c.c_str(),
		"#include <stdio.h>\nint main(void)\n{\n\tprintf(\"main ran\\n\");\n\treturn 0;\n}\n");
	var obj = format("{}/main.o", dir);
	rc = system(format("{} --no-config {} --emit-object {} > {}", cp, main_c, obj, out));
	long quiet = system(format("test ! -s {}", out));
	println("emit-object: rc={} built={} stdout_empty={}", rc,
		php::file_exists(obj.c_str()) ? 1 : 0, quiet == 0 ? 1 : 0);

	system(format("{} --help | head -1", cp));
	rc = system(format("{} --help=json | grep -q '\"name\": \"output\"'", cp));
	println("help json: rc={}", rc);

	var sh = format("{}/complete.sh", dir);
	php::file_put_contents(sh.c_str(),
		"source <(\"$MADC\" --completion=bash)\nCOMP_WORDS=(madc --dump-so)\nCOMP_CWORD=1\n_madc_complete 2>/dev/null\necho \"completion: ${COMPREPLY[@]}\"\n");
	system(format("MADC={} bash {}", cp, sh));

	system(format("rm -rf {}", dir));
	return 0;
}
```

Create `tests/testcli_option_engine.expect`:

```
unknown: rc=1 madc: error: unrecognized command-line option '-fbogus'
near: rc=1 madc: error: unrecognized command-line option '--dump-sorce'; did you mean '--dump-source'?
missing: rc=1 madc: error: missing argument to '-o'
pass-through: rc=1 madc: -Wp,-MD,x.d: not supported (madc has no separate assembler or preprocessor to pass it to)
colour empty: rc=1 Unknown -fdiagnostics-color= value:  (auto, always, never)
gcc std: 201710
dashdash: ran
response file: 42
script argv: -o out @args -- -v
emit-object: rc=0 built=1 stdout_empty=1
Usage: madc [options] <source> [program-args...]
help json: rc=0
completion: --dump-source
```

Create `tests/testcli_option_engine.exe_skip`:

```
drives the madc binary through system(); the JIT run is the test
```

Create `tests/testcli_option_engine.win64_skip`:

```
POSIX shell redirections, bash completion and a file named -dash.mad
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bash scripts/remote_build.sh sync build`, restore `hello.cpp`, then `ssh -p 2299 dev@localhost 'cd /workspace/madc && bash scripts/run_tests.sh testcli_option_engine'`.
Expected: FAIL. The `unknown:` line shows madc trying to open `-fbogus` as a file, and `gcc std:`, `dashdash:`, `response file:`, `help json:` and `completion:` are missing or wrong.

- [ ] **Step 3: `Program::set_stdlib_flavor` takes the flavor name**

Search first (Rule #4). Concept: setting the C++ stdlib flavor by name. Run: `git grep -n -E 'set_stdlib_flavor' -- src include`. Expected: the option-taking `set_stdlib_flavor_option` and three callers. The CLI loop passes the whole word, while two callers (`src/madc.cpp:1560`, `src/madc_cir.cpp:8011`) rebuild `"-stdlib=" + name` only so the setter can strip it again. Now that the reader owns the `-stdlib=` spelling, the setter takes the name.

In `include/madc.h:7486`, replace

```cpp
    bool set_stdlib_flavor_option(const madc::dis::istring &arg);
```

with

```cpp
    bool set_stdlib_flavor(const madc::dis::istring &name);	// false: unknown, or not built here
```

In `src/lexer.cpp`, replace the body at 5699–5708

```cpp
bool Program::set_stdlib_flavor_option(const madc::dis::istring &arg)
{
    static const madc::dis::istring opt = "-stdlib=";
    if ( arg.compare(0, opt.size(), opt) != 0 )
	return false;
    const madc_stdlib_flavor *f = madc_stdlib_flavor_lookup(arg.substr(opt.size()));
    if ( !f )
	return false;	// caller diagnoses with stdlib_flavor_names()
    stdlib_flavor = f;
    return true;
}
```

with

```cpp
bool Program::set_stdlib_flavor(const madc::dis::istring &name)
{
    const madc_stdlib_flavor *f = madc_stdlib_flavor_lookup(name);
    if ( !f )
	return false;	// caller diagnoses with stdlib_flavor_names()
    stdlib_flavor = f;
    return true;
}
```

In `src/madc_cir.cpp:8011`, replace `!prog.set_stdlib_flavor_option("-stdlib=" + tu.stdlib_option)` with `!prog.set_stdlib_flavor(tu.stdlib_option)`.
In `src/madc.cpp:1560`, replace `!prog->set_stdlib_flavor_option("-stdlib=" + config.stdlib_option)` with `!prog->set_stdlib_flavor(config.stdlib_option)`.

- [ ] **Step 4: Replace the ladder with the reader**

In `src/madc.cpp`:

1. Add `#include "madc_cli.h"	// madc's own command-line table` beside `#include "madc_capabilities.h"`. If `madc_console.h` is not included yet, add it (`git grep -n madc_console.h -- src/madc.cpp`).
2. Delete `print_usage` together with its two-line comment (lines 272–508).
3. Delete `warning_selection_option`, `stage_option` and `compiler_line_source` with their comments (lines 560–597). `read_linker_options` stays; the switch calls it.
4. Replace everything from `    int filearg = 1;` (line 943) through the end of the artifact-argv block (`        filearg = 1;\n    }`, line 1459) with:

```cpp
    const char *emit_object_path = NULL;
    const char *emit_executable_path = NULL;
    const char *generic_output_path = NULL;
    const char *emit_function_name = NULL;
    bool emit_pch = false;
    bool dump_cir = false;        // --dump-cir: dump CIR tree before checking
    bool dump_nodes = false;      // --dump-nodes: dump cir_node tree via madc walker
    bool dump_source = false;     // --dump-source / -E: full-fidelity source reconstruction
    bool dump_checked = false;    // --dump-cir-checked: post-check tree dump (c2m -d stage)
    bool do_emit = false;         // --emit=c11|mc11: render cir_node tree as C, no run
    CirEmitLang emit_lang = celC11;
    const char *project_manifest = NULL;  // --project <compile_commands.json>
    std::vector<std::string> cli_include_dirs; // -I as given: a --project build adds them to every TU
    std::vector<std::string> cli_define_args;  // -D as given: likewise
    std::vector<std::string> link_libs;   // -l<name>: TARGET spellings (madc_modules) opened RTLD_GLOBAL
    std::vector<std::string> cc_link_args; // -l<name> → DT_NEEDED in AOT mode
    MadcLinkPaths link_paths;             // -L<dir> (the -l search) and -Wl,-rpath,<dir> (DT_RUNPATH)
    bool compile_object = false;          // -c: emit a relocatable .o, no run
    bool emit_shared = false;             // -shared: emit an ET_DYN .so, no run
    bool no_pie = false;                  // -no-pie: fixed-base ET_EXEC (default = PIE, gcc parity)
    bool emit_relocatable = false;        // -r: relocatable link output — ONE .o (gcc/ld -r), no run
    bool show_help = false;               // --help / -h / -?
    bool help_json = false;               // --help=json
    bool show_completion = false;         // --completion=SHELL
    madc::cli::shell completion_shell = madc::cli::shell::bash;
    bool show_version = false;            // --version / -V
    bool show_machine = false;            // -dumpmachine
    bool show_capabilities = false;       // --capabilities=json
    bool show_stats = false;               // --show-stats: print input/token traffic counters
    const char *freeze_path = NULL;       // --freeze= / --freeze-append=: forest container out
    bool freeze_append = false;           // --freeze-append=: placement 2 (append to binary)
    bool freeze_mir_cache = false;        // --freeze-mir-cache: pack the compiled MIR module blob
    std::vector<std::string> freeze_ledger_srcs;  // --freeze-ledger=: AOT ledger sources (S5)
    std::vector<madc::dis::istring> freeze_also_std;  // --freeze-also-std=: more standards the profile serves
    std::vector<std::string> freeze_source_lists;  // --freeze-sources=: lists of files for the source arm
    bool run_frozen = false;              // --run-frozen[=path]: thaw + run, no parse
    const char *run_frozen_path = NULL;   // NULL = the blob appended to this executable
    bool freeze_run = false;              // --freeze-run: freeze, then re-exec fresh to run
    bool dump_forest = false;             // --dump-forest[=path]: print container surfaces
    const char *forest_relayout_path = NULL;  // --forest-relayout=<image>: the pack layout pass
    const char *dump_forest_path = NULL;  // NULL = the blob appended to this executable
    bool dump_registered = false;         // --dump-registered: post-parse name maps (oracle side B)
    bool dump_macro_table = false;        // -dM: effective macro table after lex (gcc -dM -E analogue)
    bool no_forest_bind = false;          // --no-forest-bind: force live parse (overrides the default and --forest-bind)
    bool forest_bind_asked = false;       // --forest-bind[=PATH]: bind even while freezing (a test-harness request)
    const char *config_path = NULL;       // --config=<file>: this madc.ini instead of the lookup chain
    bool no_config = false;               // --no-config: skip the madc.ini lookup entirely
    bool cli_set_std = false;             // --std= came from the COMMAND LINE (so a madc.ini `std` key must not override it)
    madc::dis::istring cli_std_option;    // that --std='s standard name: a command-line project's units compile under it
    bool interactive = false;             // -i / --interactive: the REPL, whatever stdin is (D20)
    bool lsp = false;                     // --lsp: the Language Server Protocol on stdio (split arc 1C.12)
    bool mcp = false;                     // --mcp: the Model Context Protocol on stdio (split arc 1C)
    std::string history_file;             // --history-file=PATH|no: the REPL's history (D23)
    bool history_file_given = false;

    // AOT (-c / -shared / -r / -o / --emit-*): compile through gen
    // object-capture mode and write a native artifact instead of running.
    // -o without -c or -shared means a linked executable (gcc semantics);
    // --emit-pch keeps its own -o meaning (.madh output path).
    // -E / -dM stop after preprocessing: -o names that text's file, not a
    // native artifact.
    auto preprocess_only = [&]() { return dump_source || dump_macro_table; };
    auto native_artifact_requested = [&]() {
        return compile_object || emit_shared || emit_relocatable
            || emit_object_path || emit_executable_path
            || (generic_output_path && !emit_pch && !preprocess_only());
    };

    // madc's command line, read by THE reader against madc's table
    // (src/madc_cli.cpp: the spellings, the lane rule, @file); each option
    // applies here, in command-line order. The values point into `parsed`,
    // which lives until main() returns.
    const madc::cli::result parsed =
        madc::cli::parse(madc_cli_table(), std::vector<std::string>(argv + 1, argv + argc));
    if ( !parsed.ok() )
    {
        std::cerr << "madc: error: " << parsed.message << std::endl;
        return 1;
    }
    for ( size_t k = 0; k < parsed.options.size(); k++ )
    {
        const madc::cli::occurrence &o = parsed.options[k];
        const char *value = o.value.c_str();
        // An optional value's bare word (--run-frozen) is not its empty
        // value (--run-frozen=).
        const bool bare = o.spelled && o.word == o.spelled;
        switch ( (cli_option)o.id )
        {
        case cli_option::none:
            break;
        case cli_option::verbose:
            madc_verbose = true;
            break;
        case cli_option::opt_level:
            // Bare -O == -O1; the row's choices admit 0..3.
            madc_opt_level = o.value.empty() ? 1 : o.value[0] - '0';
            break;
        case cli_option::debug_info:
            madc_debug_info = true;
            break;
        case cli_option::no_warnings:
            madc_no_warnings = true;	// gcc -w: c2mir's warnings included
            break;
        case cli_option::compile_object:
            compile_object = true;
            break;
        case cli_option::shared:
            emit_shared = true;
            break;
        case cli_option::relocatable:
            emit_relocatable = true;
            break;
        case cli_option::no_pie:
            no_pie = true;
            break;
        case cli_option::pie:
            no_pie = false;
            break;
        case cli_option::gui_subsystem:
            // Emit-lane state like -static-libmadc; ELF / Mach-O emits ignore
            // it; the --project lane takes the manifest's "kind" instead.
            madc_gui_subsystem = true;
            break;
        case cli_option::console_subsystem:
            madc_gui_subsystem = false;
            break;
        case cli_option::emit_object:
            emit_object_path = value;
            break;
        case cli_option::emit_executable:
            emit_executable_path = value;
            break;
        case cli_option::output:
            generic_output_path = value;
            break;
        case cli_option::include_dir:
            prog->add_include_dir(value);	// guards empty + normalizes trailing '/'
            if ( *value )
                cli_include_dirs.push_back(value);
            break;
        case cli_option::define:
            // NAME, NAME=VALUE (a bare NAME is "1", gcc). Object-like only;
            // applied after the builtin defines so a -D can override one.
            prog->add_cli_define(value);	// guards empty + splits NAME[=VALUE]
            if ( *value )
                cli_define_args.push_back(value);
            break;
        case cli_option::emit_pch:
            emit_pch = true;
            break;
        case cli_option::emit_function:
            emit_function_name = value;
            break;
        case cli_option::no_includes:
            prog->skip_includes = true;
            break;
        case cli_option::no_embedded_headers:
            // Header partition (madc-header-partition-handoff.md): bypass the
            // embedded SYSTEM-library shims so <...> reads the real headers
            // on disk, keeping madc-own and compiler-owned headers
            // (embedded_header_is_system_library_shim). Rides the engine so
            // --project units inherit it. The ONE switch that sends the core
            // system headers to disk.
            engine.registration_policy.bypass_system_library_headers = true;
            prog->registration_policy.bypass_system_library_headers = true;
            engine.registration_policy.system_headers_from_libmadc = false;
            prog->registration_policy.system_headers_from_libmadc = false;
            break;
        case cli_option::no_posix_compat:
            // Rides the engine so --project units inherit the target surface.
            engine.registration_policy.enable_posix_compat = false;
            prog->registration_policy.enable_posix_compat = false;
            break;
        case cli_option::no_auto_load:
            // `import` / #load bind to the program's own symbol scope; the
            // library is linked, never opened (link explicitly via -l).
            // Rides the engine so --project units inherit it.
            engine.registration_policy.enable_auto_library_loading = false;
            prog->registration_policy.enable_auto_library_loading = false;
            break;
        case cli_option::instrument_functions:
            prog->instrument_functions = true;
            break;
        case cli_option::no_builtin:
            prog->disabled_builtin_names.insert(value);	// a joined value: never empty
            break;
        case cli_option::no_eval_shims:
            // Whether this artifact can be host-called through the value ABI
            // is a property of the BUILD (for a .o only the build knows), as
            // -fPIC is: emit-lane state every lane sees.
            madc_no_eval_shims = true;
            break;
        case cli_option::eval_shims:
            madc_no_eval_shims = false;
            break;
        case cli_option::pic:
            break;	// madc's code is position-independent already
        case cli_option::diagnostics_color:
        {
            // gcc's -fdiagnostics-color[=WHEN]: the bare flag is always.
            const char *when = bare ? "always" : value;
            madc::colour_when code;
            if ( !madc::parse_colour_when(when, code) )
            {
                std::cerr << "Unknown -fdiagnostics-color= value: " << when
                          << " (auto, always, never)" << std::endl;
                return 1;
            }
            madc::set_colour_when(code);
            break;
        }
        case cli_option::no_diagnostics_color:
            madc::set_colour_when(madc::colour_when::never);
            break;
        case cli_option::dump_cir:
            dump_cir = true;
            break;
        case cli_option::dump_nodes:
            dump_nodes = true;
            break;
        case cli_option::dump_cir_checked:
            dump_checked = true;
            break;
        case cli_option::dump_source:
        case cli_option::preprocess:
            // -E: madc preprocesses during tokenize(), so the token stream IS
            // the preprocessed unit; reconstruct_source prints it,
            // content-only (no `# line` markers), diffable against gcc -E.
            dump_source = true;
            break;
        case cli_option::show_stats:
            show_stats = true;
            break;
        case cli_option::freeze:
            freeze_path = value;
            break;
        case cli_option::freeze_append:
            freeze_path = value;
            freeze_append = true;
            break;
        case cli_option::freeze_mir_cache:
            freeze_mir_cache = true;
            break;
        case cli_option::freeze_ledger:
            // Repeatable; the canonical list is scripts/ledger_sources.txt,
            // handed over by scripts/forest_pack.sh — never spelled out here.
            freeze_ledger_srcs.push_back(o.value);
            break;
        case cli_option::freeze_also_std:
            freeze_also_std.push_back(o.value);
            break;
        case cli_option::freeze_sources:
            freeze_source_lists.push_back(o.value);
            break;
        case cli_option::freeze_run:
            freeze_run = true;
            break;
        case cli_option::no_sysroot_includes:
            // The toolchain-only include view, so a freeze on a host with a
            // real SDK makes the header-less consumer's corpus. Rides the
            // engine too.
            engine.registration_policy.enable_sysroot_includes = false;
            prog->registration_policy.enable_sysroot_includes = false;
            break;
        case cli_option::pack_forest:
            madc_pack_forest_path = value;
            break;
        case cli_option::static_libmadc:
            madc_static_libmadc = true;
            break;
        case cli_option::run_frozen:
            run_frozen = true;
            if ( !bare )
                run_frozen_path = value;
            break;
        case cli_option::dump_forest:
            dump_forest = true;
            if ( !bare )
                dump_forest_path = value;
            break;
        case cli_option::forest_bind:
            prog->registration_policy.enable_forest_bind = true;
            forest_bind_asked = true;
            if ( !bare )
                prog->forest_bind_path = value;
            break;
        case cli_option::forest_touch_log:
            prog->forest_touch_log = value;
            break;
        case cli_option::forest_relayout:
            forest_relayout_path = value;	// the remainder is its logs
            break;
        case cli_option::no_forest_bind:
            no_forest_bind = true;
            break;
        case cli_option::config:
            config_path = value;
            break;
        case cli_option::no_config:
            no_config = true;
            break;
        case cli_option::dump_registered:
            dump_registered = true;
            break;
        case cli_option::dump_macros:
            dump_macro_table = true;
            break;
        case cli_option::project:
            project_manifest = value;
            break;
        case cli_option::interactive:
            interactive = true;
            break;
        case cli_option::mcp:
            mcp = true;
            break;
        case cli_option::lsp:
            lsp = true;
            break;
        case cli_option::history_file:
            history_file = o.value;	// `no`: history for the session alone
            if ( history_file == "no" )
                history_file.clear();
            history_file_given = true;
            break;
        case cli_option::help:
            show_help = true;
            help_json = o.value == "json";
            break;
        case cli_option::completion:
            show_completion = madc::cli::parse_shell(o.value, completion_shell);
            break;
        case cli_option::version:
            show_version = true;
            break;
        case cli_option::dump_machine:
            show_machine = true;
            break;
        case cli_option::capabilities:
            // json is the only format this build implements; any other (the
            // bare word included) is refused loudly rather than answered
            // with a schema the consumer did not ask for.
            if ( o.value != "json" )
            {
                std::cerr << "Unknown capabilities format: " << o.word
                          << " (json)" << std::endl;
                return 1;
            }
            show_capabilities = true;
            break;
        case cli_option::library:
        {
            // The NAME's TARGET spelling comes from the ONE owner
            // (madc_modules); JIT: opened RTLD_GLOBAL below; AOT: the
            // spelling joins the link closure verbatim.
            std::string lib = madc_module_library_spelling(value);
            cc_link_args.push_back(lib);
            link_libs.push_back(lib);
            break;
        }
        case cli_option::library_dir:
            if ( *value )
                link_paths.lib_dirs.push_back(value);
            break;
        case cli_option::linker_option:
        {
            // gcc's pass-through to the linker; madc is its own linker.
            std::string refused;
            if ( !read_linker_options(value, link_paths.runpath, refused) )
            {
                std::cerr << "madc: " << o.word << ": linker option '"
                          << refused << "' is not supported (-rpath is)"
                          << std::endl;
                return 1;
            }
            break;
        }
        case cli_option::warning_selection:
            break;	// madc's diagnostics are one fixed set
        case cli_option::warnings_as_errors:
            std::cerr << "madc: " << o.word << ": not supported (madc's warnings"
                      << " are never errors)" << std::endl;
            return 1;
        case cli_option::pass_through:
            std::cerr << "madc: " << o.word << ": not supported (madc has no separate"
                      << " assembler or preprocessor to pass it to)" << std::endl;
            return 1;
        case cli_option::emit:
            // Render the cir_node tree (MC11-IR) as C source; do not run.
            if ( !cir_emit_lang_of(value, emit_lang) )
            {
                std::cerr << "Unknown --emit target: " << value
                          << " (" << CIR_EMIT_TARGETS << ")" << std::endl;
                return 1;
            }
            do_emit = true;
            break;
        case cli_option::language_std:
            if ( !prog->set_language_standard(o.value) )
            {
                std::cerr << "Unknown --std target: " << o.value << std::endl;
                return 1;
            }
            cli_set_std = true;
            cli_std_option = o.value;
            break;
        case cli_option::stdlib_flavor:
            // Which flavors exist is a property of the BUILD host, so name the
            // ones this binary has rather than the ones the flag could spell.
            if ( !prog->set_stdlib_flavor(o.value) )
            {
                std::cerr << "Unknown -stdlib flavor: " << o.value
                          << " (this madc was built with: " << prog->stdlib_flavor_names()
                          << ")" << std::endl;
                return 1;
            }
            break;
        }
    }

    // The program's own words: the operands a line that builds took (gcc's
    // argument permutation), then the remainder; argv[filearg..argc) as before.
    std::vector<std::string> program_words(parsed.operands);
    program_words.insert(program_words.end(), parsed.remainder.begin(),
                         parsed.remainder.end());
    std::vector<char *> program_argv(1, argv[0]);
    for ( size_t k = 0; k < program_words.size(); k++ )
        program_argv.push_back(&program_words[k][0]);
    program_argv.push_back(NULL);
    argv = program_argv.data();
    argc = (int)program_words.size() + 1;
    int filearg = 1;
```

5. Replace the help block (lines 1503–1507)

```cpp
    if ( show_help )
    {
        print_usage(argv[0]);
        return 0;
    }
```

with

```cpp
    // The help, --help=json and the completion scripts are madc's table
    // (src/madc_cli.cpp) through the reader's writers: one source for all.
    if ( show_help )
    {
        if ( help_json )
            std::cout << madc::cli::describe_json(madc_cli_table()) << std::endl;
        else
            madc::cli::write_help(std::cout, madc_cli_table(), madc::terminal_columns());
        return 0;
    }

    if ( show_completion )
    {
        madc::cli::write_completion(std::cout, madc_cli_table(), completion_shell);
        return 0;
    }
```

6. Replace the `repl_options` lines (2006–2008)

```cpp
    // The REPL's options — every argument before the first positional — are
    // its Windows backend's command line (run_repl).
    const std::vector<std::string> repl_options(argv + 1, argv + filearg);
```

with

```cpp
    // The REPL's options — madc's own option words, as the reader consumed
    // them — are its Windows backend's command line (run_repl).
    const std::vector<std::string> repl_options(parsed.option_words);
```

- [ ] **Step 5: `--capabilities` reads introspection from the table**

In `src/madc_capabilities.cpp`, add `#include "madc_cli.h"	// introspection: madc's rows tagged mt_introspection` beside the other includes. In the header comment, replace

```
 * native_outputs and introspection have NO shared registry to derive from (the
 * MadcNativeKind enum names its kinds, not their CLI -c/-o/-shared/-r
 * spellings; the --dump-* set is scattered arg handling), so they are
 * CLI-surface literals here by design: this file owns the CLI's own surface.
```

with

```
 *   - introspection comes from madc's command-line table (src/madc_cli.cpp):
 *     the rows tagged mt_introspection, by their long spellings.
 *
 * native_outputs has NO shared registry to derive from (the MadcNativeKind
 * enum names its kinds, not their CLI -c/-o/-shared/-r spellings), so it is
 * a CLI-surface literal here by design.
```

Replace

```cpp
    m["introspection"] = { "dump-source", "dump-cir", "dump-nodes",
			   "dump-cir-checked", "dump-forest", "dump-registered",
			   "show-stats" };
```

with

```cpp
    m["introspection"] = json::array();
    const madc::cli::table &cli = madc_cli_table();
    for ( size_t k = 0; k < cli.nrows; k++ )
	if ( cli.rows[k].tags & mt_introspection )
	    m["introspection"].push_back(std::string(cli.rows[k].spellings[0].text).substr(2));
```

- [ ] **Step 6: Run the targeted tests to verify they pass**

Run: `bash scripts/remote_build.sh sync build`, restore `hello.cpp`. Then run `ssh -p 2299 dev@localhost 'cd /workspace/madc && bash scripts/run_tests.sh testcli_option_engine testcli_compiler_line testcli_preprocess_output testcli_undefined_reference testdiagcolour_badarg'`.
Expected: all PASS.

Run: `ssh -p 2299 dev@localhost 'cd /workspace/madc && bin/test_madc_cli && bin/test_cli_options && bash scripts/capabilities_json_gate.sh'`.
Expected: both doctest binaries print `SUCCESS`, and the gate prints `capabilities_json_gate: OK`. Its manifest's `introspection` array is unchanged.

Run the madc side of the Task 2 oracle: `ssh -p 2299 dev@localhost 'cd /workspace/madc && touch tmp/cli_empty.c && bin/madc --no-config -dM @tmp/cli_rsp.txt tmp/cli_empty.c | grep -E "^#define (A|B|C|D) " | sort'`.
Expected: exactly gcc's four lines from Task 2 Step 1.

- [ ] **Step 7: Tier 1 + Tier 2, gates, unit tests, batch**

Run: `TESTS='testcli_* testdiagcolour* testinc*' bash scripts/remote_build.sh sync build fix`.
Run: `bash scripts/remote_build.sh gates`. This covers every repository gate, among them `capabilities_json_gate`, `libcxx_gate`, `forest_config_gate` and `forest_pack_gate`, which drive madc's options by name.
Run: the static gate loop. If `check-istring-fields.sh` reports a changed count for `src/madc.cpp`, re-record it as in Task 1 Step 5. The new `std::string`s are argv text, the "OS-boundary lists" kind.
Run: `bash scripts/remote_build.sh batch`. This is the whole `tests/` suite on the JIT: every test's `.flags` now goes through the reader. Record its `tests-jit` tally on the NAS with `scripts/lane_ledger.sh record tests-jit <tally>`.
Expected: green. A red test is read before anything is changed. Each one is either a behaviour this plan changes on purpose (Global Constraints) or a defect in the reader or the switch. Fix a defect at the reader; never widen the table to swallow an error.

- [ ] **Step 8: Commit**

Stage `src/madc.cpp src/madc_capabilities.cpp include/madc.h src/lexer.cpp src/madc_cir.cpp tests/testcli_option_engine.*` (plus `scripts/istring_keep.tsv` if it was re-recorded) and dispatch `commit-writer`, asking for a CHANGELOG entry. Pointers:
- `git diff --cached`;
- the reducer `tests/testcli_option_engine.mad` (its comment names the oracle; its `.expect` is the correct output);
- this plan's "Deliberate behaviour changes" list, as the user-visible facts.

---

### Task 8: The gate, the docs, the knowledge graph

**Files:**
- Create: `scripts/check-one-option-parser.sh`
- Create: `docs/language/command-line.md`
- Modify: `AGENTS.md` (the Rule #4 standing instances)
- Modify: `docs/plans/2026-10-16-cli-options-design.md` (status line), `docs/plans/2026-10-15-dialect-faces-arc.md` (the arc's status)

**Interfaces:**
- Consumes: Task 7's `main()` (no hand-rolled argv option reads left).
- Produces: the gate `scripts/check-one-option-parser.sh`, picked up by the static gate loop and `make gates` through `scripts/check-*.sh`.

- [ ] **Step 1: Write the gate**

Create `scripts/check-one-option-parser.sh`:

```bash
#!/bin/bash
# check-one-option-parser.sh — a command line is read by ONE reader,
# madc::cli::parse (include/libmadc/cli_options.h), over a table of rows
# (madc's own: src/madc_cli.cpp).
#
# Until 2026-10 madc's main() read its options with 83 strcmp/strncmp arms,
# each deciding on its own what an option looks like and where the
# program's argv begins; three gcc-parity bugs in one week came from arms
# that disagreed (-E ignored -o, a compiler line ran as a script, -Wall was
# read as a file). Design: docs/plans/2026-10-16-cli-options-design.md.
#
# Rule: outside the reader (src/cli_options.cpp), no C/C++ source in src/ or
# include/ compares an argv word with an option spelling (strcmp(argv[i],
# "-x"), strncmp(argv[i], "--x=", n)) or tests an argv word's leading dash
# (argv[i][0] == '-'). Comparing an operand with "-" (stdin) is not an
# option read and passes. include/doctest.h is third-party.
#
# Negative control: a synthetic file with each shape must FAIL and a clean
# one must PASS — else the gate itself is broken and fails loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

PATTERN='strn?cmp *\( *argv *\[[^]]*] *, *"-[^"]|argv *\[[^]]*] *\[ *0 *] *== *'"'"'-'"'"

# scan FILE... — prints each hand-rolled option read; 0 = clean.
scan() {
	local out
	out=$(grep -n -H -E "$PATTERN" "$@" 2>/dev/null)
	[ -z "$out" ] && return 0
	echo "$out"
	return 1
}

tmpd=$(mktemp -d)
printf 'void a(char **argv, int i) { if (strcmp(argv[i], "-v") == 0) return; }\n' > "$tmpd/bad1.cpp"
printf 'void b(char **argv, int i) { if (strncmp(argv[i], "--std=", 6) == 0) return; }\n' > "$tmpd/bad2.cpp"
printf "void c(char **argv, int i) { if (argv[i][0] == '-') return; }\n" > "$tmpd/bad3.cpp"
printf 'void d(char **argv, int f) { if (strcmp(argv[f], "-") == 0) return; }\n' > "$tmpd/good.cpp"
for bad in bad1 bad2 bad3; do
	if scan "$tmpd/$bad.cpp" >/dev/null; then
		rm -rf "$tmpd"
		echo "check-one-option-parser: NEGATIVE CONTROL FAILED — $bad passed" >&2
		exit 2
	fi
done
if ! scan "$tmpd/good.cpp" >/dev/null; then
	rm -rf "$tmpd"
	echo "check-one-option-parser: POSITIVE CONTROL FAILED — the stdin operand test was flagged" >&2
	exit 2
fi
rm -rf "$tmpd"

files=$(git ls-files --cached --others --exclude-standard 'src/*.cpp' 'src/*.h' 'include/*.h' \
	'include/libmadc/*.h' 'include/madcdis/*.h' \
	| grep -v -E '^(src/cli_options\.cpp|include/doctest\.h)$')
# shellcheck disable=SC2086
if ! out=$(scan $files); then
	echo "check-one-option-parser: a command line read past madc::cli::parse:" >&2
	echo "$out" >&2
	echo "  -> add a row to the program's table (madc: src/madc_cli.cpp) and a case to its switch" >&2
	exit 1
fi
echo "check-one-option-parser: OK (every command line reads through madc::cli::parse)"
```

- [ ] **Step 2: Run the gate, then prove it bites**

Run: `bash scripts/check-one-option-parser.sh`
Expected: `check-one-option-parser: OK (every command line reads through madc::cli::parse)`. If it lists `src/madc.cpp` lines, Task 7 left an arm; move it into the table.

Then prove it fails on the ladder. HEAD is Task 7's commit, so its parent still has the old ladder:
- Run: `git worktree add tmp/gate_check HEAD~1`
- Run: `cp scripts/check-one-option-parser.sh tmp/gate_check/scripts/`
- Run: `bash tmp/gate_check/scripts/check-one-option-parser.sh`. The script `cd`s to its own tree.
- Expected: exit 1, listing the old ladder's `strcmp(argv[i], "-…")` / `strncmp(argv[i], "-…", n)` arms in `src/madc.cpp`, and not line 2010's `strcmp(argv[filearg], "-")`.
- Run: `git worktree remove --force tmp/gate_check`

- [ ] **Step 3: Write the user doc**

Create `docs/language/command-line.md`:

```markdown
# The command line

madc reads its command line the way gcc does, on Linux, macOS and Windows
alike. Its options are one table (`src/madc_cli.cpp`) read by one reader
(`include/libmadc/cli_options.h`); `madc --help`, `madc --help=json` and the
shell completions are written from the same table.

## Syntax

- `-o FILE`: the value is the next word, whatever it holds (`-o -x` names the
  file `-x`, as gcc does).
- `-IDIR` or `-I DIR`; `-DNAME[=VALUE]` or `-D NAME`; `-LDIR` or `-L DIR`.
- `-lNAME`, `-Wl,...`, `-fno-builtin-NAME`, `-O[LEVEL]`, `-W<name>`: the value
  is glued on.
- `--std=VALUE` (gcc's `-std=VALUE` too), `-stdlib=VALUE`, `--emit=VALUE`, and
  the other `NAME=VALUE` options: the value follows the `=`.
- `--run-frozen[=FILE]`, `--dump-forest[=FILE]`, `--forest-bind[=FILE]`,
  `-fdiagnostics-color[=WHEN]`, `--help[=json]`: the value may be left out.
- When more than one spelling could read a word, a whole-word spelling wins,
  then `NAME=VALUE`, then the longest glued spelling: `-Werror=x` is
  `-Werror`, not `-W`.
- No abbreviations: `--verb` is not `--verbose`.
- `--` ends madc's options.
- On a line that builds, with `-c`, `-o`, `-shared`, `-r`, `-E`, `-dM`,
  `--emit-object`, `--emit-executable` or `--dump-source`, options may follow
  the sources, as gcc's do. The same holds on a compiler line: a C or C++
  source with one of those options after it.
- On a line that runs a program, the program's file ends madc's options. Every
  later word is the program's argv, untouched: `madc tool.mad -o out` runs
  `tool.mad` with `-o out`.

## Response files

`@FILE` is replaced by FILE's words, as gcc does it:
- blanks separate words, and `'...'` and `"..."` quote;
- a backslash makes the next character literal everywhere, inside quotes too.
  Write paths with `/`, or double each `\`.

A file may name another file. One that names itself stops with
`too many @-files encountered`. A name that cannot be opened stays as written.
Response files expand only among madc's options, never inside a program's argv.

## Errors

madc exits 1 and names the problem as gcc does:

- `madc: error: unrecognized command-line option '-fbogus'`, with
  `; did you mean '--dump-source'?` when a spelling is close;
- `madc: error: missing argument to '-o'`;
- `madc: error: invalid value '4' for '-O': expected one of 0, 1, 2, 3`.

## Help, JSON and completions

- `madc --help`: wrapped to `$COLUMNS`, or 80 when it is unset.
- `madc --help=json`: every option as data (name, spellings and their forms,
  metavar, help, type, choices), grouped by section. Tools and madcide read
  this instead of copying the table.
- `madc --completion=SHELL`, for bash, zsh, fish or powershell:
  - bash: `madc --completion=bash > ~/.local/share/bash-completion/completions/madc`
  - zsh: `madc --completion=zsh > ~/.zfunc/_madc`, with `~/.zfunc` on `fpath`
  - fish: `madc --completion=fish > ~/.config/fish/completions/madc.fish`
  - PowerShell: add `madc --completion=powershell | Out-String | Invoke-Expression`
    to `$PROFILE`
```

- [ ] **Step 4: Name the owner in AGENTS.md**

In `AGENTS.md`, Top 10 Rule #4's standing instances, after the sentence that ends `madc's own lib directory first (gated by \`check-one-library-opener.sh\`);`, insert:

```
a command line is read by ONE reader, `madc::cli::parse`
(`include/libmadc/cli_options.h`), over a table of rows — madc's own is
`src/madc_cli.cpp` — and its help, `--help=json` and completions are written
from the same rows (gated by `check-one-option-parser.sh`);
```

- [ ] **Step 5: Status lines and the knowledge graph**

In `docs/plans/2026-10-16-cli-options-design.md`, replace the status line `Status: DESIGN (owner-approved direction, 2026-10-16). Spec for review.` with `Status: slices 1–2 LANDED (plan docs/plans/2026-10-16-cli-options-plan.md); slices 3–6 open.`

In `docs/plans/2026-10-15-dialect-faces-arc.md`, append this bullet after the last bullet of `## Status`. `<date>` is the landing date in `YYYY-MM-DD` form:

```markdown
- Command-line options, slices 1–2 (<date>; plan
  `docs/plans/2026-10-16-cli-options-plan.md`): the one option reader
  `madc::cli::parse` (`include/libmadc/cli_options.h`) — gcc/GNU syntax,
  `@file`, typed values, environment / config / default sources, help,
  `--help=json`, bash / zsh / fish / PowerShell completions — and madc's own
  command line as one table (`src/madc_cli.cpp`) and one `switch`; the 83-arm
  ladder deleted; gate `check-one-option-parser.sh`; DupFamily
  `command_line_option_arms` consolidated (this release's `Consolidated:` line).
```

Record the family in madc-knowledge. DupFamily nodes carry `name`, `status`, `updated_at`, `marker`, `sites` and `description` (checked against `bare_function_name_call_or_address` on 2026-10-16). `<hash>` is Task 7's commit (`git log -1 --format=%h` at that commit), and `<date>` is the landing date:

Run: `scripts/kg_query.sh "MERGE (d:DupFamily {name:'command_line_option_arms'}) SET d.status = 'consolidated <hash> <date>: one reader madc::cli::parse over the table src/madc_cli.cpp; gated check-one-option-parser.sh', d.updated_at = '<date>', d.marker = 'strcmp or strncmp of an argv word against an option spelling outside src/cli_options.cpp', d.sites = 83, d.description = 'madc main() read its command line with 83 strcmp and strncmp arms, each deciding its own value form and where the program argv begins; three gcc-parity bugs in one week came from arms that disagreed. Owner: madc::cli::parse (include/libmadc/cli_options.h) over the madc table (src/madc_cli.cpp); help, help=json and completions are written from the same rows.'"`

Keep the argument free of backticks, `$` and `!`.

- [ ] **Step 6: Run the gate loop and commit**

Run: `bash scripts/remote_build.sh sync`, restore `hello.cpp`, then the static gate loop on the container. Expected: green, with the new gate among them.
Stage `scripts/check-one-option-parser.sh docs/language/command-line.md AGENTS.md docs/plans/2026-10-16-cli-options-design.md docs/plans/2026-10-15-dialect-faces-arc.md`. Dispatch `commit-writer` for the CHANGELOG entry and its release-rule line, `Consolidated: command_line_option_arms — gate scripts/check-one-option-parser.sh`. Pointers: `git diff --cached`; `.claude/rules/branching.md`'s consolidation bullet; `scripts/check-release-consolidation.sh`. Run `bash scripts/check-release-consolidation.sh` after the commit. Expected: OK.

---

## After the plan

- The seam battery (`bash scripts/seam_battery.sh`, from the NAS) runs once, at the arc's release boundary, not per task. Its parse-cost stage checks that the reader added no start-up cost past the 0.5% tolerance. Its exe + obj, win64 and macOS stages run the CLI tests on every platform lane.
- The next plans, one per slice: Windows UTF-8 (the activeCodePage manifest), `madc::sys.flags` / `sys.orig_argv` and the table face for madcide, `python::argparse` (it brings positional rows), and the typed `madc::cli` face (member annotations).

---

## Execution record (2026-10-16)

Executed inline (executing-plans), Tasks 1–8, commits effa62e53..5c4d93bf4;
one whole-branch review (fresh reviewer, the most capable model) over
5c1532862..5c4d93bf4 — "with fixes": 0 critical, 4 important, 9 minor; one
declined line re-graded to a finding. Fix pass: d26c9f74b, d49a60180,
b13284687.

### Rulings made during execution (each with its cost if wrong)

- T1, T4: doctest 2.5 cannot decompose a prvalue pointer compared with NULL;
  such values are bound to named locals, same assertions (none).
- T2: the third new @file case is a negative guard that passes before
  expansion exists; kept, it must stay green (none).
- T2–T7: a task that adds built text to a core file re-records
  `scripts/istring_keep.tsv`, after confirming only its rows moved (a baseline
  row).
- T3: madcgit's relpath rewrote every `\` to `/` on every OS; consolidated onto
  `host_path_generic` in its own commit 90c9cb246 with a reducer (none).
- T3: `host_path_generic` got its dialect face `madc::path_generic` and its
  `dialect_faces.tsv` row, per the dialect-faces law (one face).
- T5: `terminal_columns()` ignores a non-positive or non-numeric `COLUMNS`, as
  gcc does; the echo's `COLUMNS=0` case changes from 0 to 80 (a caret layout
  under `COLUMNS=0`).
- T7: forest_index_oracle red on `__madc_bswap16/32` (from e67a8dac4); two
  Class 2 allowlist rows in dfb9d0b79 (none).
- T8: the ladder held 82 option arms, not 83 (the 83rd strcmp is the stdin
  sentinel); the gate's proof ran on 6ae4b007c; the user doc's did-you-mean
  example is `--verb` → `--verbose`, verified on the binary (prose only).
- Review: important 1–4 fixed as graded; fix 1 also adds
  `scripts/check-gates-wired.sh` (one small gate).
- Review: a non-regular `@file` stays as written and a regular one is read for
  its size (`@/dev/null` stays a word where gcc expands it to nothing).
- Review: a standard option word reads through `madc_cli_standard_of`; the
  session's `--std=` option-word protocol keeps its shape (a later
  consolidation).
- Review: the CHANGELOG's `-fbogus` suggestion example was a factual error,
  corrected (none).
- Review, declined lines: backslash escapes in `@file` (libiberty parity),
  gcc's no-op options still errors (they come in with real-codebase builds,
  section F), exit status 1 (gcc), madc.ini not on `sources` (scope), nargs
  and Windows UTF-8 argv (slices 5 and 3), the 2000-expansion memory growth
  (libiberty parity) — all stand. `--lsp --stdio` refused was re-graded a
  regression and fixed (an editor could not start madc's server).

### Deferred minors

- `madc x.c @flags` with `-o` only inside `flags` runs `x.c`: the
  compiler-line peek reads unexpanded words (gcc expands `@file`s first).
- gcc value forms not yet rows: `-ofoo`, `-l m`, `-Os`/`-Og`/`-Ofast`,
  `-g3`/`-ggdb`/`-gdwarf-N`, single-dash `-finstrument-functions` (redis's
  build passes `-ggdb`: section F).
- bash completion offers files, not directories, after `-I`/`-L`; bash's
  `COMP_WORDBREAKS` splits `--std=c<TAB>`.
- fish `complete -s '?'` for `-?` is unverified (no fish on any host).
- `--help=` with an empty value prints the plain help (gcc: missing argument).
- `check_required` counts a negated occurrence as present; `exclusive` and
  `requires_rest` read different sources — to be stated in the header.
- The option-parser gate misses argv aliases (`const char *a = argv[i]`).
- A UTF-8 BOM at the start of a response file joins the first word (gcc
  shares it).

### Found on the way

- KG Gap `shebang_standard_keeps_dialect_predefined_macros` (SILENT): a script
  whose shebang says `--std=c17` compiles as C17 but sees `__cplusplus` 201703
  and no `__STDC_VERSION__`. Fixed in 6a21775f5 (`tests/testshebang_std_macros`).
- The arc's compile-time growth: 71f64745f copied a paint set per macro
  argument; ead3e9752 recovers part of it (c_headers 257.19M → 254.64M on
  madc-release); the rest is being profiled.

### The command line, old against new

`scripts/cli_differential.sh` over `scripts/cli_differential.corpus` (160
command lines — every row's spellings and value forms, options before, after
and around sources, `@file`s, `--`, stdin, misspellings), 6ae4b007c (the
ladder) against b13284687: 120 identical, 40 differ. Every difference is
intended: a new row or value form (completions, `--help=json`, gcc's `-std=`,
`-W`, `@file`, `--`); gcc's errors where the ladder said "Failed to open file"
or ran on (missing argument, invalid value, unrecognized option with its
did-you-mean); the help's one width rule; an option after the source read as
an option; the `--lsp --stdio` / `--mcp --stdio` review fix. None is a
regression. The batch lane on ead3e9752: 2082 passed, 0 failed, 9 skipped.
