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
	const cli::match operand = cli::match_word(t, "file.c");
	CHECK(operand.option == NULL);
	const cli::match stdin_operand = cli::match_word(t, "-");
	CHECK(stdin_operand.option == NULL);
	const cli::row *std_row = cli::find_row(t, t_std);
	REQUIRE(std_row != NULL);
	CHECK(std::string(std_row->name) == "std");
	const cli::row *no_row = cli::find_row(t, 999);
	CHECK(no_row == NULL);
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
