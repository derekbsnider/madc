// B8 (plan D26): a diagnostic's source echo lays its line out in gcc's screen
// columns — a tab runs to the next 8-column stop, a UTF-8 character is one
// column and an East Asian wide one two (madc::line_layout, the one layout
// rule) — and the caret sits under its byte's screen column. Before the fix
// the line echoed with its tab raw and the caret was placed by counting bytes
// as spaces: 7 columns left of the token under a tab indent, one column right
// per extra byte of an earlier UTF-8 character.
//
// Oracle: gcc 13 (-fsyntax-only) on each line below as line 2 of a C file,
// `foo` the token it rejects; gcc cites the screen column and draws the caret
// there with the tab expanded:
//   "\tint x = 1 foo; return x; }"                   2:19
//   "\tchar *s = \"éé\"; int x = 1 foo; return x; }"   2:35
//   "    char *s = \"中\"; int x = 1 foo; return x; }"  2:31
// The byte column handed to the echo is foo's first byte (1-based); the
// caret's screen column is gcc's.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <list>
#include <vector>
#include <queue>
#include <stack>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

static const std::string CARET = "\033[1;32m^\033[m";

// The echo of `ln` with the caret at byte column `col`: the echoed line, and
// the caret's screen column (the spaces before it) — -1 when the caret line
// is not blanks then the caret.
static long echo(const std::string &ln, int col, std::string &shown)
{
    std::ostringstream os;
    show_error_source_line(ln, col, os);
    std::string out = os.str();
    size_t nl = out.find('\n');
    REQUIRE(nl != std::string::npos);
    shown = out.substr(0, nl);
    std::string caret = out.substr(nl + 1);
    size_t at = caret.find(CARET);
    if ( at == std::string::npos
	 || caret.find_first_not_of(' ') != at
	 || caret.substr(at + CARET.size()) != "\n" )
	return -1;
    return (long)at;
}

TEST_CASE("B8: the caret sits under its byte's screen column (gcc's 2:19)")
{
    setenv("COLUMNS", "200", 1);
    std::string shown;

    // A tab indent: the echo shows the tab as spaces to column 8, and the
    // caret is at gcc's screen column 19 (18 blanks before it).
    CHECK(echo("\tint x = 1 foo; return x; }", 12, shown) == 18);
    CHECK(shown == "        int x = 1 foo; return x; }");

    // Two-byte characters earlier on the line are one column each: gcc 2:35.
    CHECK(echo("\tchar *s = \"\xc3\xa9\xc3\xa9\"; int x = 1 foo; return x; }", 30, shown) == 34);
    CHECK(shown == "        char *s = \"\xc3\xa9\xc3\xa9\"; int x = 1 foo; return x; }");

    // An East Asian wide character is two columns: gcc 2:31.
    CHECK(echo("    char *s = \"\xe4\xb8\xad\"; int x = 1 foo; return x; }", 32, shown) == 30);

    // A line without tabs or multi-byte characters keeps its byte column.
    CHECK(echo("int x = 1 foo;", 11, shown) == 10);
    CHECK(shown == "int x = 1 foo;");
}

TEST_CASE("B8: the echo never throws and never walks off the line")
{
    setenv("COLUMNS", "200", 1);
    std::string shown;
    // Column 1 and a missing column put the caret at the line's start.
    CHECK(echo("\tfoo", 1, shown) == 0);
    CHECK(echo("\tfoo", 0, shown) == 0);
    // A column past the fetched line (macro-expansion provenance) is clamped
    // to the line's end: after the tab's 8 columns and foo's 3.
    CHECK(echo("\tfoo", 99, shown) == 11);
    // A control byte shows as ^X, two columns, and the caret counts them.
    CHECK(echo("a\x01" "b", 3, shown) == 3);
    CHECK(shown == "a^Ab");
}

TEST_CASE("B8: a line wider than the terminal shows its tail from the caret")
{
    setenv("COLUMNS", "20", 1);
    std::string shown;
    // "  ..." then the line from the caret's byte; the caret under the tail's
    // first column, after the five columns of the lead.
    CHECK(echo("int a_long_name = 1 foo; return a_long_name;", 21, shown) == 5);
    CHECK(shown == "  ...foo; return a_long_name;");
    unsetenv("COLUMNS");
}

// A Throw keeps its raising token's POSITION, never the token: the catch arm
// records the diagnostic after the unwind, when the token may be gone. On
// g++.dg/cpp0x/implicit7.C the cited token was a stack local of the
// ClassPattern resolver, and record_throw_diagnostic read it 13 KB below the
// stack pointer (valgrind: "Invalid read ... on thread 1's stack").
static void raise_on_a_dying_token(throwstream &ts, bool with_end)
{
    TokenIdent tok;		// dies when this frame unwinds
    tok.file = "f.c";
    tok.line = 3;
    tok.column = 7;
    if ( with_end )
    {
	tok.lex_end_line = 3;
	tok.lex_end_column = 10;
    }
    ts(&tok) << "use of undeclared identifier 'nope'" << std::flush;
}

TEST_CASE("Throw keeps the raising position past the token's lifetime")
{
    DiagnosticRenderMute mute;		// sync throws without rendering
    throwstream ts;
    CHECK_THROWS(raise_on_a_dying_token(ts, true));
    const ParsePosition *at = ts.at();
    REQUIRE((at != NULL));
    CHECK(std::string(at->file) == "f.c");
    CHECK(at->line == 3);
    CHECK(at->column == 7);
    CHECK(at->end_line == 3);
    CHECK(at->end_column == 10);
    CHECK(ts.str() == "use of undeclared identifier 'nope'");
    // No recorded end: derived from the spelling (none here: the start).
    CHECK_THROWS(raise_on_a_dying_token(ts, false));
    REQUIRE((ts.at() != NULL));
    CHECK(ts.at()->end_line == 3);
    CHECK(ts.at()->end_column == 7);
    // An error that names no token has no position.
    CHECK_THROWS(ts(NULL) << "no token" << std::flush);
    CHECK((ts.at() == NULL));
}

// D26 part 2: a diagnostic's HEADER prints the same place as gcc's — the
// start byte's screen column (madc_screen_column), while the stored column
// stays a byte count. The oracles are gcc's headers for the reducers above.
TEST_CASE("D26: a header's column is the start byte's screen column (gcc's)")
{
    CHECK(madc_screen_column("\tint x = 1 foo; return x; }", 12) == 19);
    CHECK(madc_screen_column("\tchar *s = \"\xc3\xa9\xc3\xa9\"; int x = 1 foo; return x; }", 30) == 35);
    CHECK(madc_screen_column("    char *s = \"\xe4\xb8\xad\"; int x = 1 foo; return x; }", 32) == 31);
    CHECK(madc_screen_column("int x = 1 foo;", 11) == 11);
    // Past the line's end (an end-of-input cite): one column per byte on.
    CHECK(madc_screen_column("\tfoo", 6) == 13);
    CHECK(madc_screen_column("\tfoo", 1) == 1);
    CHECK(madc_screen_column("", 3) == 3);
}

// D26: the caret underlines the cited token, gcc's `^~~` — `^` under its
// first byte, then a `~` for each further screen column through its last
// byte (show_error_source_line's end_col). The oracles are gcc 13's echoes
// (-fsyntax-only) of each line as a C file's:
//   "\tint x = 1 foo; return x; }"   2:19  ^~~
//   "    int x = 1 \"中\";"           2:15  ^~~~   (a wide character, 2 columns)
//   "    int y = 1 \"éé\";"           3:15  ^~~~   (two-byte characters, 1 each)
//   "    return undeclared_name; }"   4:12  ^~~~~~~~~~~~~~~
// The indent and the mark of the caret line for byte columns col..end_col.
static std::string underline(const std::string &ln, int col, int end_col,
			     long &indent)
{
    std::ostringstream os;
    show_error_source_line(ln, col, os, end_col);
    std::string out = os.str();
    size_t nl = out.find('\n');
    REQUIRE(nl != std::string::npos);
    std::string caret = out.substr(nl + 1);
    const std::string open = "\033[1;32m", close = "\033[m\n";
    size_t at = caret.find(open);
    indent = -1;
    if ( at == std::string::npos || caret.find_first_not_of(' ') != at
	 || caret.size() < at + open.size() + close.size()
	 || caret.compare(caret.size() - close.size(), close.size(), close) )
	return "";
    indent = (long)at;
    return caret.substr(at + open.size(),
			caret.size() - close.size() - at - open.size());
}

TEST_CASE("D26: the caret underlines the token (gcc's ^~~)")
{
    setenv("COLUMNS", "200", 1);
    long indent;
    CHECK(underline("\tint x = 1 foo; return x; }", 12, 14, indent) == "^~~");
    CHECK(indent == 18);
    CHECK(underline("    int x = 1 \"\xe4\xb8\xad\";", 15, 19, indent) == "^~~~");
    CHECK(indent == 14);
    CHECK(underline("    int y = 1 \"\xc3\xa9\xc3\xa9\";", 15, 20, indent) == "^~~~");
    CHECK(indent == 14);
    CHECK(underline("    return undeclared_name; }", 12, 26, indent)
	  == "^~~~~~~~~~~~~~~");
    CHECK(indent == 11);
    // A one-byte token, an end before the start, and no end: the caret alone.
    CHECK(underline("int x = 1 ; }", 11, 11, indent) == "^");
    CHECK(underline("int x = 1 foo;", 11, 3, indent) == "^");
    CHECK(underline("int x = 1 foo;", 11, 0, indent) == "^");
    // An end past the line (a token that runs on) stops at the line's end.
    CHECK(underline("x + undeclared_fo\\", 5, 99, indent) == "^~~~~~~~~~~~~~");
    CHECK(indent == 4);
    // A line wider than the terminal: the tail's caret is underlined too.
    setenv("COLUMNS", "20", 1);
    CHECK(underline("int a_long_name = 1 foo; return a_long_name;", 21, 23, indent)
	  == "^~~");
    CHECK(indent == 5);
    unsetenv("COLUMNS");
}

// A token's end on the cited line (madc_underline_end): its own line's end
// column, the line's length when it runs onto a later line, none before.
TEST_CASE("D26: the underline's last byte on the cited line")
{
    CHECK(madc_underline_end("return undeclared_name;", 2, 8, 2, 22) == 22);
    CHECK(madc_underline_end("x + undeclared_fo\\", 1, 5, 2, 1) == 18);
    CHECK(madc_underline_end("abc", 2, 1, 0, 0) == 0);
    CHECK(madc_underline_end("abc", 2, 3, 2, 1) == 0);
}
