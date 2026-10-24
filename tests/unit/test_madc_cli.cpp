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

TEST_CASE("an LSP client's --stdio names the transport, not a file")
{
	// LSP client libraries append --stdio for a stdio transport; madc's
	// servers speak only on stdin/stdout, so the word is theirs to accept
	// (the ladder before the table read it as the file and served).
	cli::result r = parse({ "--lsp", "--stdio" });
	CHECK(r.ok());
	CHECK(r.operands.empty());
	CHECK(r.remainder.empty());
	r = parse({ "--mcp", "--stdio", "x.mad" });
	CHECK(r.ok());
	CHECK(r.remainder == words({ "x.mad" }));
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
