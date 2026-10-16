// Unit tests for the command-line option reader (include/libmadc/cli_options.h;
// design docs/plans/2026-10-16-cli-options-design.md). Oracles: gcc's own
// option handling for the gcc-shaped rules (separate / joined / the longest
// spelling, "missing argument to", "unrecognized command-line option"), GNU
// getopt for bundling, `--` and permutation.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <fstream>
#include <initializer_list>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
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
	const cli::occurrence *level_default = r.last(s_level);
	REQUIRE(level_default != NULL);
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
	const cli::occurrence *level_unset = r.last(s_level);
	CHECK(level_unset == NULL);

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
	const cli::occurrence *fast_env = r.last(s_fast);
	REQUIRE(fast_env != NULL);
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
