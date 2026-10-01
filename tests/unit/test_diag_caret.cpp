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
