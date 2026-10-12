// The syntax-colour owners (split arc 1B.9, design
// docs/plans/2026-10-10-prompt-colour-design.md): the theme (madc's one
// built-in default, the `.theme` text a host's profiles are written in — the
// fixtures in tests/unit/themes/ are madcide's profiles as of the split — the
// parent rule), the style-to-SGR step and the terminal's depth, and the
// classifier's C++ face under a standard.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "highlight_class.h"
#include "madc_theme.h"
#include "madcdis/ui_sgr.h"
#include "madc/bits/file_kinds"		// madc::fkC17 / fkCPP17: standard codes

using madc::hub::ui_style;
using madc::hub::ui_colour_depth;

static ui_style spec(const char *s)
{
    ui_style st;
    REQUIRE(madc::hub::ui_style_of(s, st));
    return st;
}

TEST_CASE("highlight_class_of reads back every class's name")
{
    for ( std::size_t i = 0; i < highlight_class_count; ++i )
    {
	HighlightClass c = HighlightClass::hcNone;
	REQUIRE(highlight_class_of(highlight_class_name((HighlightClass)i), c));
	CHECK((std::size_t)c == i);
    }
    HighlightClass c;
    CHECK_FALSE(highlight_class_of("keywrd", c));
}

TEST_CASE("theme_default is madcide's default.theme (JOE parity), with the parent rule")
{
    const madc::theme &t = madc::theme_default();
    CHECK(t.of(HighlightClass::hcKeyword) == spec("bold"));
    CHECK(t.of(HighlightClass::hcControl) == spec("bold"));
    CHECK(t.of(HighlightClass::hcString) == spec("cyan"));
    CHECK(t.of(HighlightClass::hcNumber) == spec("cyan"));
    CHECK(t.of(HighlightClass::hcComment) == spec("green"));
    CHECK(t.of(HighlightClass::hcDirective) == spec("blue"));
    CHECK(t.of(HighlightClass::hcIncludePath) == spec("bold cyan"));
    CHECK(t.of(HighlightClass::hcNamespace) == spec("bold"));	// typename's
    CHECK(t.of(HighlightClass::hcIdent).is_normal());
    CHECK(t.of(HighlightClass::hcFunction).is_normal());
    for ( std::size_t i = 0; i < (std::size_t)madc::hub::tui_chrome::count; ++i )
	CHECK_FALSE(t.chrome_named[i]);				// the grid's own
    std::ifstream f("../tests/unit/themes/default.theme");
    REQUIRE(f);
    std::ostringstream text;
    text << f.rdbuf();
    madc::theme read;
    std::string err;
    REQUIRE(madc::theme_read(text.str(), read, err));
    for ( std::size_t i = 0; i < highlight_class_count; ++i )
	CHECK(read.syntax[i] == t.syntax[i]);
}

TEST_CASE("theme_read: the profile fixtures read unchanged; refusals name the line")
{
    const char *profiles[] = { "classic", "vivid", "vscode" };
    for ( const char *p : profiles )
    {
	std::ifstream f((std::string("../tests/unit/themes/") + p + ".theme").c_str());
	REQUIRE(f);
	std::ostringstream text;
	text << f.rdbuf();
	madc::theme t;
	std::string err;
	const bool ok = madc::theme_read(text.str(), t, err);
	const std::string why = std::string(p) + ": " + err;
	CHECK_MESSAGE(ok, why);
    }
    madc::theme t;
    std::string err;
    REQUIRE(madc::theme_read("# a scheme\nkeyword red\n\n@gui bg #000000\n", t, err));
    CHECK(t.of(HighlightClass::hcKeyword) == spec("red"));
    CHECK(t.of(HighlightClass::hcControl) == spec("red"));	// the parent rule
    CHECK(t.of(HighlightClass::hcDirective) == spec("red"));
    CHECK(t.of(HighlightClass::hcString).is_normal());		// a whole scheme
    REQUIRE(madc::theme_read("divider blue\nstatusbar inverse\n", t, err));
    CHECK(t.chrome_named[(std::size_t)madc::hub::tui_chrome::divider]);
    CHECK(t.chrome[(std::size_t)madc::hub::tui_chrome::divider] == spec("blue"));
    CHECK_FALSE(t.chrome_named[(std::size_t)madc::hub::tui_chrome::gutter]);
    CHECK(t.of(HighlightClass::hcKeyword).is_normal());	// no lines: no colour
    madc::theme kept = t;
    CHECK_FALSE(madc::theme_read("keyword bold\nkeywrd bold\n", t, err));
    CHECK(err == "line 2: no class or chrome named 'keywrd'");
    CHECK_FALSE(madc::theme_read("string chartreuse\n", t, err));
    CHECK(err == "line 1: 'string' has no style the theme can read");
    CHECK_FALSE(madc::theme_read("comment\n", t, err));
    for ( std::size_t i = 0; i < highlight_class_count; ++i )
	CHECK(t.syntax[i] == kept.syntax[i]);			// unchanged on refusal
}

TEST_CASE("sgr: the one style-to-SGR step, per depth")
{
    std::string out;
    madc::hub::sgr(out, ui_style(), spec("bold cyan"), ui_colour_depth::ansi16);
    CHECK(out == "\x1b[1;36m");
    out.clear();
    madc::hub::sgr(out, spec("bold cyan"), ui_style(), ui_colour_depth::ansi16);
    CHECK(out == "\x1b[0m");
    out.clear();
    madc::hub::sgr(out, ui_style(), spec("#ff8000"), ui_colour_depth::truecolor);
    CHECK(out == "\x1b[38;2;255;128;0m");
    out.clear();
    madc::hub::sgr(out, ui_style(), spec("#ff8000"), ui_colour_depth::xterm256);
    CHECK(out == "\x1b[38;5;" + std::to_string(madc::hub::ui_rgb_nearest_256(0xff8000)) + "m");
}

#ifndef _WIN32
TEST_CASE("terminal_colour_depth reads COLORTERM, then TERM")
{
    unsetenv("WT_SESSION");
    setenv("COLORTERM", "truecolor", 1);
    CHECK(madc::hub::terminal_colour_depth() == ui_colour_depth::truecolor);
    unsetenv("COLORTERM");
    setenv("TERM", "xterm-256color", 1);
    CHECK(madc::hub::terminal_colour_depth() == ui_colour_depth::xterm256);
    setenv("TERM", "xterm", 1);
    CHECK(madc::hub::terminal_colour_depth() == ui_colour_depth::ansi16);
}
#endif

static HighlightClass class_at(const std::vector<madc::highlight_span> &spans,
			       long line, long column)
{
    for ( const madc::highlight_span &s : spans )
	if ( s.line == line && s.column == column )
	    return s.cls;
    return HighlightClass::hcNone;
}

TEST_CASE("lex_classes: lex spans as enums, under the caller's standard")
{
    std::vector<madc::highlight_span> spans;
    madc::lex_classes("int class = 42; /* c */\nreturn \"a\\n\";", madc::fkC17, spans);
    CHECK(class_at(spans, 1, 0) == HighlightClass::hcType);
    CHECK(class_at(spans, 1, 4) == HighlightClass::hcIdent);	// C17: a name
    CHECK(class_at(spans, 1, 12) == HighlightClass::hcNumber);
    CHECK(class_at(spans, 1, 16) == HighlightClass::hcComment);
    CHECK(class_at(spans, 2, 0) == HighlightClass::hcControl);
    CHECK(class_at(spans, 2, 7) == HighlightClass::hcString);
    madc::lex_classes("int class = 42;", madc::fkCPP17, spans);
    CHECK(class_at(spans, 1, 4) == HighlightClass::hcKeyword);	// C++: a keyword
    madc::lex_classes("x = \"open", madc::fkC17, spans);	// an incomplete entry lexes
    CHECK_FALSE(spans.empty());
}

TEST_CASE("theme_marks: runs of one style, an escape inside its string")
{
    std::vector<madc::hub::line_mark> marks;
    std::vector<std::string> lines = { "int s = \"a\\n\"; /* c", "c */ x" };
    madc::theme_marks(madc::theme_default(), lines, madc::fkC17,
		      ui_colour_depth::ansi16, marks);
    REQUIRE(marks.size() == 6u);
    CHECK((marks[0].line == 0 && marks[0].from == 0 && marks[0].to == 3));	// int
    CHECK(marks[0].on == "\x1b[1m");
    CHECK((marks[1].from == 8 && marks[1].to == 10));		// "a
    CHECK(marks[1].on == "\x1b[36m");
    CHECK((marks[2].from == 10 && marks[2].to == 12));		// \n
    CHECK(marks[2].on == "\x1b[1;36m");
    CHECK((marks[3].from == 12 && marks[3].to == 13));		// "
    CHECK((marks[4].line == 0 && marks[4].from == 15 && marks[4].to == 19));	// /* c
    CHECK(marks[4].on == "\x1b[32m");
    CHECK((marks[5].line == 1 && marks[5].from == 0 && marks[5].to == 4));	// c */
}

// A shown text's code coloured (split arc 1B.9): each range lexed alone —
// a range across lines lexes as one text — the labels between them as they
// are; a range that runs backward or past the text is skipped.
TEST_CASE("theme_paint: the code ranges coloured, the rest as it is")
{
    const std::string text = "Members:   int x\n           int y\nDefined:   @ REPL[1]:1";
    const std::vector<madc::code_range> code = {
	{ 11, 16 }, { 28, 33 }, { 20, 25 }, { 40, 200 }
    };
    CHECK(madc::theme_paint(madc::theme_default(), text, code, madc::fkC17,
			    ui_colour_depth::ansi16)
	  == "Members:   \x1b[1mint\x1b[0m x\n           \x1b[1mint\x1b[0m y\n"
	     "Defined:   @ REPL[1]:1");
    const std::vector<madc::code_range> across = { { 11, 33 } };
    CHECK(madc::theme_paint(madc::theme_default(), text, across, madc::fkC17,
			    ui_colour_depth::ansi16)
	  == "Members:   \x1b[1mint\x1b[0m x\n           \x1b[1mint\x1b[0m y\n"
	     "Defined:   @ REPL[1]:1");
    CHECK(madc::theme_paint(madc::theme_default(), text, {}, madc::fkC17,
			    ui_colour_depth::ansi16) == text);
}
