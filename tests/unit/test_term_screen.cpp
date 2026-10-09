// Unit battery for madcdis/term_screen.h — the bounded terminal screen
// madcide's embedded Terminal tab folds a pty's bytes through (polish
// P3b-2): printables at the cursor, \r overwrite, \b, tabs, the dropped
// controls and escape sequences, the two CSI erases it honours, OSC
// titles, the scrollback cap, and the load/text round trip an owner uses
// to keep the state on a document between feeds.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>

#include "madcdis/term_screen.h"

using madc::hub::term_screen;

static void feed(term_screen &s, const std::string &bytes)
{
    s.feed(bytes.data(), bytes.size());
}

TEST_CASE("term_screen — lines, carriage return overwrite, backspace, tabs")
{
    term_screen s;
    feed(s, "hello\nworld");
    CHECK(s.text() == "hello\nworld");
    CHECK(s.lines.size() == 1u);
    CHECK(s.col == 5u);
    // \r returns to column 0 and the next bytes OVERWRITE (a progress bar).
    feed(s, "\rWORLD!");
    CHECK(s.text() == "hello\nWORLD!");
    // A shorter rewrite leaves the tail (a terminal does): 25% over 100%.
    term_screen p;
    feed(p, "100%\r25%");
    CHECK(p.cur == "25%%");
    // Backspace steps back; the next byte overwrites.
    term_screen b;
    feed(b, "abc\bX");
    CHECK(b.cur == "abX");
    // Tab pads to the next 8-column stop.
    term_screen t;
    feed(t, "ab\tc");
    CHECK(t.cur == "ab      c");
    CHECK(t.col == 9u);
    // A pty's \r\n is one line break.
    term_screen crlf;
    feed(crlf, "a\r\nb\r\n");
    CHECK(crlf.text() == "a\nb\n");
    CHECK(crlf.lines.size() == 2u);
}

TEST_CASE("term_screen — controls and escape sequences drop; erase-to-EOL and clear-screen act; OSC ends at BEL or ESC \\")
{
    term_screen s;
    feed(s, "\x07plain\x01\x02");		// BEL and other C0: dropped
    CHECK(s.cur == "plain");
    // SGR colours (dropped — onto the one style later) and cursor motion.
    term_screen c;
    feed(c, "\x1b[1;31mred\x1b[0m \x1b[2Aup");
    CHECK(c.cur == "red up");
    // Erase to the end of the line after a return: a spinner's clean tail.
    term_screen k;
    feed(k, "loading...\rdone\x1b[K");
    CHECK(k.cur == "done");
    // CSI 2J clears everything (a `clear`).
    term_screen j;
    feed(j, "one\ntwo\x1b[2Jthree");
    CHECK(j.text() == "three");
    // An OSC window title: dropped up to BEL, or up to ESC \.
    term_screen o;
    feed(o, "\x1b]0;my title\x07" "after");	// (a split literal: \x07a would be one escape)
    CHECK(o.cur == "after");
    term_screen o2;
    feed(o2, "\x1b]2;t\x1b\\x");
    CHECK(o2.cur == "x");
    // ESC + one byte (keypad modes): dropped, the next byte is text.
    term_screen e;
    feed(e, "\x1b=q");
    CHECK(e.cur == "q");
    // A sequence split across two feeds parses whole.
    term_screen split;
    feed(split, "ab\x1b[");
    feed(split, "Kcd");
    CHECK(split.cur == "abcd");
}

TEST_CASE("term_screen — the scrollback cap keeps the newest lines; load/text round-trip carries the state")
{
    term_screen s;
    s.cap = 3;
    feed(s, "1\n2\n3\n4\n5\ncur");
    CHECK(s.lines.size() == 3u);
    CHECK(s.text() == "3\n4\n5\ncur");
    // An owner keeps text + column between feeds: loading them back and
    // feeding more continues where the cursor was.
    term_screen a;
    feed(a, "ab\ncd\r");
    std::string kept = a.text();
    size_t kept_col = a.col;
    CHECK(kept == "ab\ncd");
    CHECK(kept_col == 0u);
    term_screen b;
    b.load(kept, kept_col);
    feed(b, "XY");
    CHECK(b.text() == "ab\nXY");
    // A column past the kept line clamps.
    term_screen cl;
    cl.load("abc", 9);
    CHECK(cl.col == 3u);
}

// The program's colours (facelift S8): SGR sets the pen, every byte keeps
// the pen it was written with, spans() hands the runs out as theme specs.
TEST_CASE("term_screen — SGR colours become span rows over the text")
{
    term_screen s;
    feed(s, "a \x1b[1;31merr\x1b[0m ok\n\x1b[38;5;208mx\x1b[38;2;1;2;3my\x1b[39m z");
    CHECK(s.text() == "a err ok\nxy z");
    std::vector<term_screen::span> sp = s.spans();
    REQUIRE(sp.size() == 3u);
    CHECK(sp[0].s == 2u);
    CHECK(sp[0].e == 5u);
    CHECK(sp[0].spec == "bold red");
    CHECK(sp[1].s == 9u);				// "x": the 256 palette's 208
    CHECK(sp[1].spec == "#ff8700");
    CHECK(sp[2].spec == "#010203");			// "y": 24-bit
    // Bright, background, inverse, and their resets.
    term_screen b;
    feed(b, "\x1b[92;44;7mq\x1b[27;49mr\x1b[0ms");
    sp = b.spans();
    REQUIRE(sp.size() == 2u);
    CHECK(sp[0].spec == "inverse #00ff00 bg_blue");
    CHECK(sp[1].spec == "#00ff00");
    // An overwrite takes the new pen; an erase drops the colour with the text.
    term_screen o;
    feed(o, "\x1b[32mabc\r\x1b[0mA\x1b[K");
    CHECK(o.text() == "A");
    CHECK(o.spans().empty());
}

TEST_CASE("term_screen — the pen and the spans round-trip through an owner")
{
    term_screen s;
    feed(s, "x\x1b[33my");
    const std::string text = s.text();
    const std::vector<term_screen::span> runs = s.spans();
    const std::string pen = s.pen_spec();
    CHECK(pen == "yellow");
    term_screen t;
    t.load(text, s.col);
    t.load_spans(runs);
    t.load_pen(pen);
    feed(t, "z\x1b[0m!");
    CHECK(t.text() == "xyz!");
    std::vector<term_screen::span> sp = t.spans();
    REQUIRE(sp.size() == 1u);
    CHECK(sp[0].s == 1u);
    CHECK(sp[0].e == 3u);				// y and z, one run
    CHECK(sp[0].spec == "yellow");
}

TEST_CASE("ui_style_spec — the inverse of the spec parser")
{
    const char *specs[] = { "normal", "bold red", "dim italic underline blink inverse",
			    "#123456 bg_#abcdef", "cyan bg_black", "bold #ff8700" };
    for ( size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); ++i )
    {
	CAPTURE(specs[i]);
	madc::hub::ui_style a, b;
	REQUIRE(madc::hub::ui_style_of(specs[i], a));
	REQUIRE(madc::hub::ui_style_of(madc::hub::ui_style_spec(a), b));
	CHECK(a == b);
	CHECK(madc::hub::ui_style_spec(a) == specs[i]);
    }
    CHECK(madc::hub::ui_xterm256_rgb(196) == 0xff0000u);
    CHECK(madc::hub::ui_xterm256_rgb(244) == 0x808080u);
    CHECK(madc::hub::ui_xterm256_rgb(9) == 0xff0000u);
}
