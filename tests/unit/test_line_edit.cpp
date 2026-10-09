// The line editor (plan §41.7a, D23): madcdis/line_edit.h's model and
// painter, and the display width they measure with (madcdis/text_utf16.h).
// Keys go through the production path: terminal bytes -> tui_keyparse ->
// ui_apply_keys over the one key owner with line_edit_bindings(). A scripted
// host answers the model's questions (Enter, Tab), as the REPL does with
// its session.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <functional>
#include <string>
#include <vector>

#include "madcdis/line_edit.h"
#include "madcdis/text_utf16.h"
#include "madcdis/tui_model.h"
#include "madcdis/ui_input.h"

using madc::hub::focus_state;
using madc::hub::key_resolver;
using madc::hub::line_action;
using madc::hub::line_edit;
using madc::hub::line_painter;
using madc::hub::line_view;
using madc::hub::tui_keyev;
using madc::hub::tui_keyparse;

namespace {

// The editor and its host: `classify` answers Enter, `words` feed Tab.
struct bench
{
    line_edit ed;
    tui_keyparse parse;
    key_resolver keys;
    focus_state focus;
    std::function<line_edit::verdict(const std::string &)> classify;
    std::vector<std::string> words;
    std::vector<std::string> taken;	// entries the host took
    unsigned finals, dropped, ended, asked;

    bench() : ed("c11> "), finals(0), dropped(0), ended(0), asked(0)
    {
	keys.set_bindings(madc::hub::line_edit_bindings());
	classify = [](const std::string &) { return line_edit::verdict::taken; };
    }

    // The host's word: identifier characters before the caret.
    size_t word_start() const
    {
	size_t i = ed.caret();
	const std::string &t = ed.text();
	while ( i > 0 && (isalnum((unsigned char)t[i - 1]) || t[i - 1] == '_') )
	    --i;
	return i;
    }

    void type(const std::string &bytes)
    {
	std::vector<tui_keyev> kv;
	parse.feed(bytes.data(), bytes.size(), kv);
	parse.flush(kv);
	ed.feed(madc::hub::ui_apply_keys(keys, focus, kv));
	for (;;)
	{
	    switch ( ed.step() )
	    {
		case line_edit::outcome::idle:
		    return;
		case line_edit::outcome::enter:
		{
		    if ( ed.enter_is_final() )
		    {
			++finals;
			taken.push_back(ed.text());
			ed.entered(line_edit::verdict::taken);
			ed.start();
			break;
		    }
		    ++asked;
		    line_edit::verdict v = classify(ed.text());
		    if ( v == line_edit::verdict::taken )
		    {
			taken.push_back(ed.text());
			ed.entered(v);
			ed.start();
		    }
		    else
			ed.entered(v);
		    break;
		}
		case line_edit::outcome::complete:
		{
		    size_t start = word_start();
		    std::string word = ed.text().substr(start, ed.caret() - start);
		    std::vector<std::string> match;
		    for ( size_t i = 0; i < words.size(); ++i )
			if ( !word.empty() && words[i].compare(0, word.size(), word) == 0 )
			    match.push_back(words[i]);
		    ed.completed(start, match);
		    break;
		}
		case line_edit::outcome::dropped:
		    ++dropped;
		    ed.start();
		    break;
		case line_edit::outcome::ended:
		    ++ended;
		    return;
	    }
	}
    }
};

// An entry with an unclosed brace is incomplete; one naming `if` with no
// `else` is extendable (the stand-in for the session's classifier).
line_edit::verdict braces(const std::string &t)
{
    int depth = 0;
    for ( size_t i = 0; i < t.size(); ++i )
	depth += t[i] == '{' ? 1 : t[i] == '}' ? -1 : 0;
    if ( depth > 0 )
	return line_edit::verdict::incomplete;
    if ( t.compare(0, 3, "if ") == 0 && t.find("else") == std::string::npos )
	return line_edit::verdict::extendable;
    return line_edit::verdict::taken;
}

} // namespace

TEST_CASE("the width owner: C++20's estimated width, UTF-8 decoding")
{
    CHECK(madc::codepoint_columns('a') == 1);
    CHECK(madc::codepoint_columns(0x10FF) == 1);
    CHECK(madc::codepoint_columns(0x1100) == 2);
    CHECK(madc::codepoint_columns(0x115F) == 2);
    CHECK(madc::codepoint_columns(0x1160) == 1);
    CHECK(madc::codepoint_columns(0x65E5) == 2);	// 日
    CHECK(madc::codepoint_columns(0x1F300) == 2);
    CHECK(madc::codepoint_columns(0x1F64F) == 2);
    CHECK(madc::codepoint_columns(0x1F650) == 1);
    CHECK(madc::codepoint_columns(0x3FFFD) == 2);
    CHECK(madc::codepoint_columns(0x3FFFE) == 1);

    uint32_t cp = 0;
    CHECK(madc::utf8_decode_at("\xc3\xa9", 0, cp) == 2);
    CHECK(cp == 0xE9);
    CHECK(madc::utf8_decode_at("\xe6\x97\xa5", 0, cp) == 3);
    CHECK(cp == 0x65E5);
    CHECK(madc::utf8_decode_at("\xf0\x9f\x8c\x80", 0, cp) == 4);
    CHECK(cp == 0x1F300);
    CHECK(madc::utf8_decode_at("\xc3", 0, cp) == 1);	// truncated
    CHECK(cp == 0xC3);
    CHECK(madc::utf8_decode_at("\xc3" "A", 0, cp) == 1);	// no continuation
    CHECK(madc::utf8_decode_at("\xa9", 0, cp) == 1);	// a stray one

    std::vector<size_t> col;
    CHECK(madc::line_layout("a\tb", 0, col) == "a       b");
    CHECK(col[2] == 8);
    CHECK(madc::line_layout("a\tb", 5, col) == "a  b");
    CHECK(madc::line_layout("\x01" "x", 0, col) == "^Ax");
    CHECK(col[1] == 2);
    CHECK(madc::line_width("\xe6\x97\xa5\xe6\x9c\xac") == 4);	// 日本
}

TEST_CASE("the bindings are readline's Emacs keys, Meta as the Esc prefix")
{
    const madc::hub::tui_bindings &b = madc::hub::line_edit_bindings();
    CHECK(b.action_of("enter").code == (int64_t)line_action::accept);
    CHECK(b.action_of("esc enter").code == (int64_t)line_action::newline);
    CHECK(b.action_of("esc b").code == (int64_t)line_action::backward_word);
    CHECK(b.action_of("^a").name == "beginning-of-line");
    CHECK(b.prefix("esc"));
    CHECK(b.seq_for_action("yank") == "^y");
}

TEST_CASE("typing and moving the caret")
{
    bench t;
    t.type("abc\x01X");			// ^A
    CHECK(t.ed.text() == "Xabc");
    t.type("\x05Y");			// ^E
    CHECK(t.ed.text() == "XabcY");
    t.type("\x1b[D\x1b[DZ");		// left left
    CHECK(t.ed.text() == "XabZcY");
    t.type("\x1b[CW");			// right
    CHECK(t.ed.text() == "XabZcWY");
    t.type("\x02\x02\x06V");		// ^B ^B ^F
    CHECK(t.ed.text() == "XabZcVWY");

    bench w;
    w.type("foo bar_baz qux");
    w.type("\x1b" "b" "\x1b" "b" "!");	// Alt+b twice (the Meta prefix in one burst)
    CHECK(w.ed.text() == "foo !bar_baz qux");
    w.type("\x1b" "f" "?");		// Alt+f
    CHECK(w.ed.text() == "foo !bar_baz? qux");
    w.type("\x1b[H<\x1b[F>");		// home, end
    CHECK(w.ed.text() == "<foo !bar_baz? qux>");
    // Esc typed alone, then the key: the two-key `esc b`.
    bench e;
    e.type("ab cd");
    e.type("\x1b");			// alone: the pause (flush) makes it esc
    e.type("b!");
    CHECK(e.ed.text() == "ab !cd");
}

TEST_CASE("deleting, killing and yanking")
{
    bench t;
    t.type("hello\x7f");		// backspace
    CHECK(t.ed.text() == "hell");
    t.type("\x01\x04");			// ^A ^D: delete under the caret
    CHECK(t.ed.text() == "ell");
    t.type("\x1b[3~");			// del
    CHECK(t.ed.text() == "ll");

    bench k;
    k.type("a b c\x17");		// ^W
    CHECK(k.ed.text() == "a b ");
    k.type("\x17");			// a second ^W joins the kill
    CHECK(k.ed.text() == "a ");
    k.type("\x19");			// ^Y
    CHECK(k.ed.text() == "a b c");
    k.type("\x01\x0b");			// ^A ^K
    CHECK(k.ed.text() == "");
    k.type("\x19\x19");
    CHECK(k.ed.text() == "a b ca b c");

    bench u;
    u.type("keep this\x15");		// ^U
    CHECK(u.ed.text() == "");
    u.type("x \x19");
    CHECK(u.ed.text() == "x keep this");
    u.type("\x1b\x7f");			// esc backspace
    CHECK(u.ed.text() == "x keep ");
    u.type("\x01\x1b" "d");		// ^A, Alt+d
    CHECK(u.ed.text() == " keep ");
}

TEST_CASE("transpose and undo")
{
    bench t;
    t.type("ab\x14");			// ^T at the end: the two before swap
    CHECK(t.ed.text() == "ba");
    bench m;
    m.type("abc\x1b[D\x1b[D\x14");	// ^T inside: before and at swap
    CHECK(m.ed.text() == "bac");
    CHECK(m.ed.caret() == 2);

    bench u;
    u.type("abc");
    u.type("\x7f");
    CHECK(u.ed.text() == "ab");
    u.type("\x1f");			// ^_
    CHECK(u.ed.text() == "abc");
    u.type("\x1f");			// the typed run is one step
    CHECK(u.ed.text() == "");
}

TEST_CASE("UTF-8: a caret never stops inside a code point")
{
    bench t;
    t.type("a\xc3\xa9");		// aé
    CHECK(t.ed.text() == "a\xc3\xa9");
    t.type("\x1b[Db");
    CHECK(t.ed.text() == "ab\xc3\xa9");
    t.type("\x1b[C\x7f");
    CHECK(t.ed.text() == "ab");
    t.type("\xe6\x97\xa5\x14");		// 日, ^T
    CHECK(t.ed.text() == "a\xe6\x97\xa5" "b");
}

TEST_CASE("Enter asks the host wherever the caret is (Julia)")
{
    bench t;
    t.classify = braces;
    t.type("int f() {\r");
    CHECK(t.taken.empty());
    CHECK(t.ed.text() == "int f() {\n");
    t.type("return 1; }\r");
    REQUIRE(t.taken.size() == 1u);
    CHECK(t.taken[0] == "int f() {\nreturn 1; }");
    CHECK(t.ed.text() == "");

    // A complete entry, the caret on its first line: Enter takes it.
    bench m;
    m.type("a;\x1b\rb;");		// esc enter: a newline, no question
    CHECK(m.asked == 0u);
    m.type("\x1b[A\r");
    REQUIRE(m.taken.size() == 1u);
    CHECK(m.taken[0] == "a;\nb;");

    // An incomplete entry, the caret mid-line: the newline goes there.
    bench c;
    c.classify = braces;
    c.type("{ab\x1b[D\r");
    CHECK(c.ed.text() == "{a\nb");
}

TEST_CASE("the third Enter in a row at the end takes an incomplete entry")
{
    bench t;
    t.classify = braces;
    t.type("{\r");
    t.type("\r");
    CHECK(t.finals == 0u);
    CHECK(t.ed.text() == "{\n\n");
    t.type("\r");
    CHECK(t.finals == 1u);
    REQUIRE(t.taken.size() == 1u);
    CHECK(t.taken[0] == "{\n\n");

    // Typing between Enters starts the count again.
    bench u;
    u.classify = braces;
    u.type("{\r\rx\r\r");
    CHECK(u.finals == 0u);
}

TEST_CASE("an extendable entry waits for its else; a blank line takes it")
{
    bench t;
    t.classify = braces;
    t.type("if (c) f();\r");
    CHECK(t.taken.empty());
    t.type("\r");
    CHECK(t.finals == 1u);
    REQUIRE(t.taken.size() == 1u);
    CHECK(t.taken[0] == "if (c) f();\n");

    bench e;
    e.classify = braces;
    e.type("if (c) f();\relse g();\r");
    CHECK(e.finals == 0u);
    REQUIRE(e.taken.size() == 1u);
    CHECK(e.taken[0] == "if (c) f();\nelse g();");
}

TEST_CASE("a paste is typing: its line breaks act as Enter, its tabs are text")
{
    bench t;
    t.type("\x1b[200~a;\r\nb;\x1b[201~");
    REQUIRE(t.taken.size() == 1u);
    CHECK(t.taken[0] == "a;");
    CHECK(t.ed.text() == "b;");

    bench tab;
    tab.words.push_back("xyz");
    tab.type("\x1b[200~\tx\x1b[201~");
    CHECK(tab.ed.text() == "\tx");

    // No three-Enter rule in a paste: blank lines stay in the entry.
    bench b;
    b.classify = braces;
    b.type("\x1b[200~{\n\n\n\x1b[201~");
    CHECK(b.finals == 0u);
    CHECK(b.ed.text() == "{\n\n\n");

    // A paste split across reads, and a pause inside it.
    bench s;
    s.type("\x1b[200~int a");
    s.type(" = 1;\n\x1b[201");
    CHECK(s.taken.size() == 1u);
    s.type("~x");
    CHECK(s.ed.text() == "x");
}

TEST_CASE("Tab indents after whitespace and completes a word")
{
    bench t;
    t.type("\t");
    CHECK(t.ed.text() == "    ");
    t.type("ab\x01\t");			// only whitespace before the caret
    CHECK(t.ed.text() == "        ab");

    bench c;
    c.words = { "abcdef" };
    c.type("ab\t");
    CHECK(c.ed.text() == "abcdef");

    bench p;
    p.words = { "abcx", "abcy", "zzz" };
    p.type("ab\t");
    CHECK(p.ed.text() == "abc");	// the common prefix
    CHECK(p.ed.take_listed().empty());
    p.type("\t");			// a second Tab in a row lists them
    std::vector<std::string> l = p.ed.take_listed();
    REQUIRE(l.size() == 2u);
    CHECK(l[0] == "abcx");
    CHECK(l[1] == "abcy");

    bench f;
    f.words = { "abcx", "abcy" };
    f.type("abc\t");
    CHECK(f.ed.take_listed().empty());	// the first Tab lists nothing
    f.type("q\x7f\t");			// not in a row
    CHECK(f.ed.take_listed().empty());

    bench n;
    n.type("zz\t");			// no candidate: nothing
    CHECK(n.ed.text() == "zz");
}

TEST_CASE("Up and Down move between the entry's lines, keeping the column")
{
    bench t;
    t.type("abcdef\x1b\rxy");
    t.type("\x1b[A");
    CHECK(t.ed.caret() == 2u);
    t.type("\x1b[B");
    CHECK(t.ed.caret() == t.ed.text().size());
    t.type("\x1b[A\x05\x1b[B");		// the column is renewed by ^E
    CHECK(t.ed.caret() == t.ed.text().size());
    // Up and Down in a row keep the column ^E left (line 0's end), as
    // Emacs's goal column does; on the first line ^P stays.
    t.type("\x10\x10");
    CHECK(t.ed.caret() == 6u);
}

TEST_CASE("Ctrl-C drops the entry, Ctrl-D ends an empty one, Ctrl-L clears")
{
    bench t;
    t.type("int x\x03");
    CHECK(t.dropped == 1u);
    CHECK(t.ed.text() == "");
    t.type("\x0c");
    CHECK(t.ed.take_clear());
    CHECK_FALSE(t.ed.take_clear());
    t.type("\x04");
    CHECK(t.ended == 1u);
}

TEST_CASE("the painter: Julia's full refresh, relative to the cursor")
{
    line_painter p;
    line_view v;
    v.prompt = "c11> ";
    v.lines.push_back("x = 1");
    v.caret_line = 0;
    v.caret_byte = 5;
    CHECK(p.paint(v, 80) == "\r\x1b[0Kc11> x = 1\r\x1b[10C");
    CHECK(p.paint(v, 80) == "\r\x1b[0Kc11> x = 1\r\x1b[10C");

    // Two lines, the caret on the first: the continuation is indented to
    // the prompt's width; the repaint climbs from the last row.
    line_painter q;
    line_view two;
    two.prompt = "c11> ";
    two.lines.push_back("int f() {");
    two.lines.push_back("return 1;");
    two.caret_line = 0;
    two.caret_byte = 3;
    CHECK(q.paint(two, 80)
	  == "\r\x1b[0Kc11> int f() {\r\n     return 1;\x1b[1A\r\x1b[8C");
    CHECK(q.paint(two, 80)
	  == "\x1b[1B\r\x1b[0K\x1b[1A\r\x1b[0K"
	     "c11> int f() {\r\n     return 1;\x1b[1A\r\x1b[8C");

    // A line wider than the terminal takes the rows it wraps to.
    line_painter w;
    line_view wide;
    wide.prompt = "> ";
    wide.lines.push_back("abcdefghij");
    wide.caret_byte = 10;
    CHECK(w.paint(wide, 10) == "\r\x1b[0K> abcdefghij\r\x1b[2C");
    CHECK(w.paint(wide, 10) == "\r\x1b[0K\x1b[1A\r\x1b[0K"
				"> abcdefghij\r\x1b[2C");

    // A last line that fills its row exactly: a line break makes the row
    // after it real, and the caret sits at its start.
    line_painter e;
    line_view exact;
    exact.prompt = "> ";
    exact.lines.push_back("abcdefgh");
    exact.caret_byte = 8;
    CHECK(e.paint(exact, 10) == "\r\x1b[0K> abcdefgh\r\n\r");

    // Wide characters count two columns.
    line_painter j;
    line_view cjk;
    cjk.prompt = "> ";
    cjk.lines.push_back("\xe6\x97\xa5\xe6\x9c\xac");
    cjk.caret_byte = 6;
    CHECK(j.paint(cjk, 80) == "\r\x1b[0K> \xe6\x97\xa5\xe6\x9c\xac\r\x1b[6C");

    // finish: the caret to the end, the tail, a new row; list: columns.
    line_painter f;
    line_view fv = two;
    CHECK(f.finish(fv, 80, "^C")
	  == "\r\x1b[0Kc11> int f() {\r\n     return 1;\r\x1b[14C^C\r\n");
    std::vector<std::string> items = { "abc", "abd", "xyz" };
    CHECK(line_painter::list(items, 20) == "abc  abd  xyz\r\n");
    CHECK(line_painter::list(items, 8) == "abc  xyz\r\nabd\r\n");
    CHECK(line_painter::fresh_row(4) == "    \r\x1b[0K");
}

TEST_CASE("history: the ring skips blanks and repeats; prefix and search lookups")
{
    madc::hub::line_history h;
    CHECK(h.add("int x = 1;"));
    CHECK_FALSE(h.add("int x = 1;"));		// a repeat of the newest
    CHECK_FALSE(h.add("  \n"));			// blank
    CHECK(h.add("x * 2"));
    CHECK(h.add("int y = x;"));
    REQUIRE(h.size() == 3u);
    CHECK(h.prefix_before("int", h.size()) == 2u);
    CHECK(h.prefix_before("int", 2) == 0u);
    CHECK(h.prefix_before("int", 0) == madc::hub::line_history::npos);
    CHECK(h.prefix_after("int", 0) == 2u);
    CHECK(h.prefix_after("zz", 0) == madc::hub::line_history::npos);
    size_t at = 0;
    CHECK(h.holding_before("x", h.size(), at) == 2u);
    CHECK(at == 8u);				// the last occurrence
    CHECK(h.holding_before("x", 1, at) == 1u);
    CHECK(h.holding_after("x", 0, at) == 0u);
    CHECK(at == 4u);				// the first occurrence
}

TEST_CASE("history: Julia's record format, written and read back")
{
    std::string r = madc::hub::history_record("2026-09-27 15:38:39Z", "c17",
					      "int f()\n{\n\n}");
    CHECK(r == "# time: 2026-09-27 15:38:39Z\n# mode: c17\n"
	       "\tint f()\n\t{\n\t\n\t}\n");
    std::string file = r
	+ madc::hub::history_record("2026-09-27 15:38:40Z", "c++17", "x")
	+ "garbage line\n"
	+ "# time: 2026-09-27 15:38:41Z\r\n# mode: madc\r\n\tv + 1\r\n";
    std::vector<std::pair<std::string, std::string> > recs =
	madc::hub::history_records(file);
    REQUIRE(recs.size() == 3u);
    CHECK(recs[0].first == "c17");
    CHECK(recs[0].second == "int f()\n{\n\n}");
    CHECK(recs[1].first == "c++17");
    CHECK(recs[1].second == "x");
    CHECK(recs[2].first == "madc");
    CHECK(recs[2].second == "v + 1");
}

TEST_CASE("Up on the first line recalls by prefix; Down walks back (Julia)")
{
    madc::hub::line_history h;
    h.add("int apple = 1;");
    h.add("x * 2");
    h.add("int banana = 2;");
    bench t;
    t.ed.set_history(&h);
    t.type("\x1b[A");				// empty prefix: the newest
    CHECK(t.ed.text() == "int banana = 2;");
    t.type("\x1b[A");
    CHECK(t.ed.text() == "x * 2");
    t.type("\x1b[B\x1b[B");			// past the newest: what was typed
    CHECK(t.ed.text() == "");

    bench p;
    p.ed.set_history(&h);
    p.type("int");
    p.type("\x1b[A");				// the prefix is the text before the caret
    CHECK(p.ed.text() == "int banana = 2;");
    CHECK(p.ed.caret() == 3u);			// the caret stays after it
    p.type("\x1b[A");
    CHECK(p.ed.text() == "int apple = 1;");
    p.type("\x1b[A");				// no older one: stays
    CHECK(p.ed.text() == "int apple = 1;");
    p.type("\x1b[B\x1b[B");
    CHECK(p.ed.text() == "int");

    // An edit keeps the recalled entry as typed; Up then starts afresh.
    bench e;
    e.ed.set_history(&h);
    e.type("\x1b[A\x05!");
    CHECK(e.ed.text() == "int banana = 2;!");
    e.type("\x1b[B");
    CHECK(e.ed.text() == "int banana = 2;!");
}

TEST_CASE("Ctrl-R searches history incrementally; Enter leaves the match unrun")
{
    madc::hub::line_history h;
    h.add("int apple = 1;");
    h.add("apple * 2");
    h.add("int banana = 2;");
    bench t;
    t.ed.set_history(&h);
    t.type("typed");
    t.type("\x12");				// ^R
    CHECK(t.ed.searching());
    CHECK(t.ed.view().prompt == "(reverse-i-search)'': ");
    t.type("apple");
    CHECK(t.ed.view().prompt == "(reverse-i-search)'apple': ");
    CHECK(t.ed.view().lines[0] == "apple * 2");
    t.type("\x12");				// the next older holding it
    CHECK(t.ed.view().lines[0] == "int apple = 1;");
    t.type("\x12");				// none older: failed, the match stays
    CHECK(t.ed.view().prompt == "(failed reverse-i-search)'apple': ");
    CHECK(t.ed.view().lines[0] == "int apple = 1;");
    t.type("\r");				// accept: in the entry, not run
    CHECK_FALSE(t.ed.searching());
    CHECK(t.ed.text() == "int apple = 1;");
    CHECK(t.ed.caret() == 4u);
    CHECK(t.taken.empty());

    // Ctrl-G restores the entry; another key keeps the match and acts.
    bench g;
    g.ed.set_history(&h);
    g.type("mine\x12" "ban\x07");		// ^G
    CHECK(g.ed.text() == "mine");
    g.type("\x12" "ban\x05!");			// ^E acts on the match
    CHECK(g.ed.text() == "int banana = 2;!");

    // A second ^R with an empty query takes the last one.
    bench q;
    q.ed.set_history(&h);
    q.type("\x12" "apple\r");
    q.type("\x03");
    q.type("\x12\x12");
    CHECK(q.ed.view().lines[0] == "apple * 2");
}
