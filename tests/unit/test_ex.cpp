// Unit battery for the ex core's command-line grammar (split arc 1B.6,
// slice 1; design docs/plans/2026-10-10-ex-core-design.md): addresses,
// ranges, the command table's abbreviations and each command's operands.
// ORACLE: vim's ex (`vim -e -s`, vim 9 `:help ex-cmd-index` and
// `:help cmdline-ranges`) — the abbreviation a row accepts, the address a
// form names; the `ka` / `g!` / `r!cmd` spellings are vim's.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>

#include "madc_ex.h"

using madc::ex::address;
using madc::ex::command;
using madc::ex::command_of;
using madc::ex::line;
using madc::ex::parse;

static command code_of(const char *name)
{
    command c = command::none;
    REQUIRE(command_of(name, c));
    return c;
}

static bool refused(const char *name)
{
    command c = command::none;
    return !command_of(name, c);
}

static line parsed(const char *text)
{
    line l;
    std::string error;
    CAPTURE(text);
    REQUIRE(parse(text, l, error));
    CHECK(error.empty());
    return l;
}

static std::string refusal(const char *text)
{
    line l;
    std::string error;
    CAPTURE(text);
    CHECK_FALSE(parse(text, l, error));
    return error;
}

TEST_CASE("ex: a typed name selects one row by vim's abbreviations")
{
    CHECK(code_of("s") == command::substitute);
    CHECK(code_of("su") == command::substitute);
    CHECK(code_of("substitute") == command::substitute);
    CHECK(code_of("d") == command::del);
    CHECK(code_of("del") == command::del);
    CHECK(code_of("p") == command::print);
    CHECK(code_of("pw") == command::pwd);
    CHECK(code_of("n") == command::number);
    CHECK(code_of("nu") == command::number);
    CHECK(code_of("l") == command::list);
    CHECK(code_of("ls") == command::buffers);
    CHECK(code_of("files") == command::buffers);
    CHECK(code_of("buffers") == command::buffers);
    CHECK(code_of("b") == command::buffer);
    CHECK(code_of("buf") == command::buffer);
    CHECK(code_of("bn") == command::bnext);
    CHECK(code_of("bp") == command::bprevious);
    CHECK(code_of("bN") == command::bprevious);
    CHECK(code_of("c") == command::change);
    CHECK(code_of("co") == command::copy);
    CHECK(code_of("t") == command::copy);
    CHECK(code_of("cd") == command::cd);
    CHECK(code_of("m") == command::move);
    CHECK(code_of("ma") == command::mark);
    CHECK(code_of("mark") == command::mark);
    CHECK(code_of("marks") == command::marks);
    CHECK(code_of("k") == command::mark);
    CHECK(code_of("r") == command::read);
    CHECK(code_of("re") == command::read);
    CHECK(code_of("red") == command::redo);
    CHECK(code_of("ret") == command::retab);
    CHECK(code_of("u") == command::undo);
    CHECK(code_of("up") == command::update);
    CHECK(code_of("w") == command::write);
    CHECK(code_of("wq") == command::write_quit);
    CHECK(code_of("x") == command::exit);
    CHECK(code_of("exi") == command::exit);
    CHECK(code_of("e") == command::edit);
    CHECK(code_of("se") == command::set);
    CHECK(code_of("sor") == command::sort);
    CHECK(code_of("sav") == command::saveas);
    CHECK(code_of("g") == command::global);
    CHECK(code_of("v") == command::vglobal);
    CHECK(code_of("h") == command::help);
    CHECK(code_of("q") == command::quit);
    // Too short for any row, or no row at all (vim: :sa is :sargument,
    // :so :source, :f :file — none of them madc's).
    CHECK(refused("sa"));
    CHECK(refused("so"));
    CHECK(refused("f"));
    CHECK(refused("xyz"));
    CHECK(refused("substitutes"));
    CHECK(refused(""));
}

TEST_CASE("ex: addresses — N . $ /re/ ?re? 'x and their offsets")
{
    line l = parsed("5");
    CHECK(l.code == command::none);
    CHECK(l.where.count == 1);
    CHECK(l.where.second.kind == address::base::line);
    CHECK(l.where.second.line == 5u);

    l = parsed(".+3-1p");
    CHECK(l.code == command::print);
    CHECK(l.where.second.kind == address::base::current);
    CHECK(l.where.second.offset == 2);

    l = parsed("$-2");
    CHECK(l.where.second.kind == address::base::last);
    CHECK(l.where.second.offset == -2);

    l = parsed("+");			// .+1
    CHECK(l.where.second.kind == address::base::none);
    CHECK(l.where.second.offset == 1);
    l = parsed("--");			// .-2
    CHECK(l.where.second.offset == -2);
    l = parsed(".5");			// vim: a number after a base adds
    CHECK(l.where.second.offset == 5);

    l = parsed("/foo(bar)?/+1d");
    CHECK(l.code == command::del);
    CHECK(l.where.second.kind == address::base::search_forward);
    CHECK(l.where.second.pattern == "foo(bar)?");
    CHECK(l.where.second.offset == 1);

    // An escaped delimiter stays escaped: PCRE2 reads `\/` and `\?` as
    // the literal characters.
    l = parsed("/a\\/b/");
    CHECK(l.where.second.pattern == "a\\/b");
    l = parsed("?x\\?y?p");
    CHECK(l.where.second.kind == address::base::search_backward);
    CHECK(l.where.second.pattern == "x\\?y");

    // An unclosed pattern runs to the end of the line (vim).
    l = parsed("/tail");
    CHECK(l.where.second.pattern == "tail");

    // \/ \? \& — the last pattern.
    l = parsed("\\/p");
    CHECK(l.where.second.kind == address::base::search_forward);
    CHECK(l.where.second.pattern.empty());
    l = parsed("\\?");
    CHECK(l.where.second.kind == address::base::search_backward);

    l = parsed("'a,'bd");
    CHECK(l.where.count == 2);
    CHECK(l.where.first.kind == address::base::mark);
    CHECK(l.where.first.mark_name == 'a');
    CHECK(l.where.second.mark_name == 'b');
    CHECK(refusal("'A") == "invalid mark: a mark is a letter a-z");
}

TEST_CASE("ex: ranges — % , ; and the missing address beside a separator")
{
    line l = parsed("%s/a/b/g");
    CHECK(l.where.count == 2);
    CHECK(l.where.first.kind == address::base::line);
    CHECK(l.where.first.line == 1u);
    CHECK(l.where.second.kind == address::base::last);

    l = parsed("1,5d");
    CHECK(l.where.count == 2);
    CHECK(l.where.first.line == 1u);
    CHECK(l.where.second.line == 5u);
    CHECK_FALSE(l.where.from_current);

    l = parsed("2;+3p");
    CHECK(l.where.from_current);
    CHECK(l.where.second.offset == 3);

    l = parsed(",5p");			// .,5
    CHECK(l.where.first.kind == address::base::current);
    CHECK(l.where.second.line == 5u);
    l = parsed("3,p");			// 3,.
    CHECK(l.where.first.line == 3u);
    CHECK(l.where.second.kind == address::base::current);

    l = parsed("1,2,3p");		// the last two count
    CHECK(l.where.first.line == 2u);
    CHECK(l.where.second.line == 3u);

    l = parsed("  ::  .,$ p");		// colons and blanks before it
    CHECK(l.code == command::print);
    CHECK(l.where.first.kind == address::base::current);

    l = parsed("");
    CHECK(l.code == command::none);
    CHECK(l.where.count == 0);
}

TEST_CASE("ex: names, the force flag and the symbol commands")
{
    line l = parsed("q!");
    CHECK(l.code == command::quit);
    CHECK(l.force);
    l = parsed("e! other.c");
    CHECK(l.code == command::edit);
    CHECK(l.force);
    CHECK(l.argument == "other.c");
    l = parsed("w! out.txt  ");
    CHECK(l.force);
    CHECK(l.argument == "out.txt");

    CHECK(parsed("=").code == command::line_number);
    CHECK(parsed("$=").code == command::line_number);
    CHECK(parsed("#").code == command::number);
    l = parsed(">>");
    CHECK(l.code == command::shift_right);
    CHECK(l.levels == 2);
    l = parsed("<");
    CHECK(l.code == command::shift_left);
    CHECK(l.levels == 1);
    l = parsed("1,3> 2");
    CHECK(l.levels == 1);
    CHECK(l.has_count);
    CHECK(l.count == 2u);

    l = parsed("ka");			// vim: the mark's letter run on
    CHECK(l.code == command::mark);
    CHECK(l.argument == "a");
    l = parsed("mark z");
    CHECK(l.argument == "z");
    CHECK(refusal("k") == "mark needs a letter a-z");

    CHECK(refusal("frobnicate") == "not an editor command: 'frobnicate'");
    CHECK(refusal("1,2@") == "not an editor command: '@'");
    CHECK(refusal("d x") == "trailing characters: 'x'");
    CHECK(refusal("p 0") == "a count must be positive");
    CHECK(refusal("u now") == "trailing characters: 'now'");
}

TEST_CASE("ex: substitute's pattern, replacement, flags and count")
{
    line l = parsed("s/a+(b)/[\\1&]/gi");
    CHECK(l.code == command::substitute);
    CHECK(l.pattern == "a+(b)");
    CHECK(l.replacement == "[\\1&]");
    CHECK(l.flags == "gi");

    // Another delimiter; an escaped delimiter in the replacement is the
    // character itself, in the pattern it stays escaped.
    l = parsed("s#x\\#y#p\\#q#");
    CHECK(l.pattern == "x\\#y");
    CHECK(l.replacement == "p#q");

    l = parsed("s/x/y/ 3");
    CHECK(l.has_count);
    CHECK(l.count == 3u);
    l = parsed("s/only");			// unclosed: no replacement
    CHECK(l.pattern == "only");
    CHECK(l.replacement.empty());

    // No delimiter: repeat the last substitute.
    l = parsed("s");
    CHECK(l.code == command::repeat_sub);
    l = parsed("&");
    CHECK(l.code == command::repeat_sub);
    l = parsed("&&");
    CHECK(l.code == command::repeat_sub_flags);
    l = parsed("%&g");
    CHECK(l.code == command::repeat_sub);
    CHECK(l.flags == "g");

    CHECK(refusal("s/a/b/c")
	  == "the c flag asks before each change, which needs normal mode");
}

TEST_CASE("ex: global, move / copy, and the shell forms")
{
    line l = parsed("g/TODO/d");
    CHECK(l.code == command::global);
    CHECK(l.pattern == "TODO");
    CHECK(l.subcommand == "d");
    l = parsed("v/keep/");
    CHECK(l.code == command::vglobal);
    CHECK(l.subcommand.empty());
    l = parsed("g!/x/ s/a/b/");		// vim: :g! is :v
    CHECK(l.code == command::vglobal);
    CHECK(l.subcommand == "s/a/b/");
    CHECK(refusal("g") == "global needs a pattern: global/re/cmd");

    l = parsed("2,4m0");
    CHECK(l.code == command::move);
    CHECK(l.destination.kind == address::base::line);
    CHECK(l.destination.line == 0u);
    l = parsed("t.");
    CHECK(l.code == command::copy);
    CHECK(l.destination.kind == address::base::current);
    l = parsed("co $-1");
    CHECK(l.destination.kind == address::base::last);
    CHECK(l.destination.offset == -1);
    CHECK(refusal("m") == "move needs a destination address");

    l = parsed("!ls -l");
    CHECK(l.code == command::shell);
    CHECK(l.argument == "ls -l");
    CHECK(refusal("!") == "! needs a command");
    l = parsed("r !date");
    CHECK(l.code == command::read);
    CHECK(l.shell_form);
    CHECK(l.argument == "date");
    l = parsed("r!date");		// vim: :r!cmd reads too
    CHECK(l.shell_form);
    CHECK(l.argument == "date");
    l = parsed("r notes.txt");
    CHECK_FALSE(l.shell_form);
    CHECK(l.argument == "notes.txt");
    l = parsed("1,3w !wc -l");
    CHECK(l.code == command::write);
    CHECK(l.shell_form);
    CHECK_FALSE(l.force);
    CHECK(l.argument == "wc -l");
}

// ---------------------------------------------------------------- session
// Slice 2: the ed core applied to a world's document. ORACLE: GNU ed and
// vim's ex (`vim -e -s`) on the same buffer — the lines a command shows,
// the text it leaves, and the current line after it.

#include "madcdis/hub.h"

using madc::ex::outcome;
using madc::ex::session;
using madc::hub::entity_id;
using madc::hub::world;

static entity_id open_doc(world &w, const std::string &text)
{
    entity_id e = w.create("document");
    REQUIRE(w.doc_attach(e, "t.c", 33, text));	// 33: any kind code
    return e;
}

static std::string body(world &w, entity_id e)
{
    return w.text_of(e)->text();
}

static std::string ran(session &s, const char *cmd)
{
    CAPTURE(cmd);
    outcome o = s.run(cmd);
    CAPTURE(o.error);
    CHECK(o.ok);
    return o.shown;
}

static std::string refused_run(session &s, const char *cmd)
{
    CAPTURE(cmd);
    outcome o = s.run(cmd);
    CHECK_FALSE(o.ok);
    return o.error;
}

static const char *FIVE = "one\ntwo\nthree\nfour\nfive\n";

TEST_CASE("ex session: printing, the current line and line numbers")
{
    world w;
    entity_id e = open_doc(w, FIVE);
    session s(w);
    CHECK(refused_run(s, "p") == "no buffer (:e FILE opens one)");
    REQUIRE(s.use(e));
    CHECK(s.current_line() == 5u);		// ex: a read file ends at $
    CHECK(ran(s, "1p") == "one\n");
    CHECK(s.current_line() == 1u);
    CHECK(ran(s, "") == "two\n");		// an empty line: the next one
    CHECK(ran(s, "=") == "5\n");		// = is $= (ed, vim)
    CHECK(ran(s, ".=") == "2\n");
    CHECK(ran(s, "2,3nu") == "  2 two\n  3 three\n");
    CHECK(ran(s, "#") == "  3 three\n");
    CHECK(ran(s, "4") == "four\n");		// a bare address prints (ex)
    CHECK(ran(s, "0") == "one\n");		// :0 is the first line
    CHECK(ran(s, "$-1,$p") == "four\nfive\n");
    CHECK(ran(s, "1p 2") == "one\ntwo\n");	// a count from the address
    CHECK(refused_run(s, "9p") == "invalid range");
    CHECK(refused_run(s, "3,1p") == "backwards range");
    s.settings().number = true;
    CHECK(ran(s, "1p") == "  1 one\n");
    s.settings().number = false;

    world w2;
    entity_id t = open_doc(w2, "a\tb\x01\n");
    session s2(w2);
    REQUIRE(s2.use(t));
    CHECK(ran(s2, "l") == "a^Ib^A$\n");
}

TEST_CASE("ex session: searches wrap; the empty pattern is the last one")
{
    world w;
    entity_id e = open_doc(w, FIVE);
    session s(w);
    REQUIRE(s.use(e));
    CHECK(ran(s, "/t(hr|w)/p") == "two\n");	// from $ it wraps to 1
    CHECK(ran(s, "//p") == "three\n");
    CHECK(ran(s, "?o?p") == "two\n");		// backward
    CHECK(ran(s, "?o?p") == "one\n");
    CHECK(ran(s, "?o?p") == "four\n");		// wraps to the end
    CHECK(refused_run(s, "/nomatch/") == "pattern not found: nomatch");
    CHECK(refused_run(s, "/(/") .find("invalid pattern '('") == 0);
}

TEST_CASE("ex session: delete, undo and redo as one group per command")
{
    world w;
    entity_id e = open_doc(w, FIVE);
    session s(w);
    REQUIRE(s.use(e));
    ran(s, "2,3d");
    CHECK(body(w, e) == "one\nfour\nfive\n");
    CHECK(s.current_line() == 2u);		// the line after the range
    ran(s, "$d");
    CHECK(body(w, e) == "one\nfour\n");
    CHECK(s.current_line() == 2u);
    ran(s, "u");
    CHECK(body(w, e) == "one\nfour\nfive\n");
    ran(s, "u");
    CHECK(body(w, e) == FIVE);
    CHECK(refused_run(s, "u") == "already at oldest change");
    ran(s, "red");
    CHECK(body(w, e) == "one\nfour\nfive\n");
    ran(s, "%d");
    CHECK(body(w, e).empty());
    CHECK(s.current_line() == 0u);
    CHECK(refused_run(s, "p") == "no lines in the buffer");

    // An unterminated last line stays unterminated.
    world w2;
    entity_id t = open_doc(w2, "a\nb\nc");
    session s2(w2);
    REQUIRE(s2.use(t));
    ran(s2, "$d");
    CHECK(body(w2, t) == "a\nb");
    ran(s2, "1d");
    CHECK(body(w2, t) == "b");

    w2.doc_set_read_only(t, true);
    CHECK(refused_run(s2, "d") == "the buffer is read-only");
}

TEST_CASE("ex session: join, move, copy")
{
    world w;
    entity_id e = open_doc(w, "a\n   b\nf(x\n)\n\nend\n");
    session s(w);
    REQUIRE(s.use(e));
    ran(s, "1j");				// :Nj joins N and N+1
    CHECK(body(w, e) == "a b\nf(x\n)\n\nend\n");
    ran(s, "2j");				// no space before `)`
    CHECK(body(w, e) == "a b\nf(x)\n\nend\n");
    ran(s, "2,3j");				// nor for an empty line
    CHECK(body(w, e) == "a b\nf(x)\nend\n");
    ran(s, "1,2j!");				// j! joins as is
    CHECK(body(w, e) == "a bf(x)\nend\n");
    CHECK(s.current_line() == 1u);

    world w2;
    entity_id t = open_doc(w2, FIVE);
    session s2(w2);
    REQUIRE(s2.use(t));
    ran(s2, "1m$");
    CHECK(body(w2, t) == "two\nthree\nfour\nfive\none\n");
    CHECK(s2.current_line() == 5u);
    ran(s2, "4,5m0");
    CHECK(body(w2, t) == "five\none\ntwo\nthree\nfour\n");
    CHECK(s2.current_line() == 2u);
    CHECK(refused_run(s2, "2,4m3")
	  == "cannot move a range of lines into itself");
    ran(s2, "1t.");				// copy after the current line
    CHECK(body(w2, t) == "five\none\nfive\ntwo\nthree\nfour\n");
    CHECK(s2.current_line() == 3u);
    ran(s2, "1,2co$");
    CHECK(body(w2, t) == "five\none\nfive\ntwo\nthree\nfour\nfive\none\n");

    world w3;
    entity_id u = open_doc(w3, "a\nb");	// unterminated
    session s3(w3);
    REQUIRE(s3.use(u));
    ran(s3, "2m0");
    CHECK(body(w3, u) == "b\na");
}

TEST_CASE("ex session: shifts by shiftwidth, tabs unless expandtab")
{
    world w;
    entity_id e = open_doc(w, "x\n\n  y\n");
    session s(w);
    REQUIRE(s.use(e));
    ran(s, "%>");
    CHECK(body(w, e) == "\tx\n\n\t  y\n");	// an empty line stays empty
    s.settings().shiftwidth = 4;
    s.settings().expandtab = true;
    ran(s, "1<");
    CHECK(body(w, e) == "    x\n\n\t  y\n");
    ran(s, "3<<");
    CHECK(body(w, e) == "    x\n\n  y\n");	// width 10 - 8, as spaces
    ran(s, "1,3< 5");				// a count starts at the last
    CHECK(body(w, e) == "    x\n\ny\n");	// address (3) and clamps at $
}

TEST_CASE("ex session: marks follow the edits around them")
{
    world w;
    entity_id e = open_doc(w, FIVE);
    session s(w);
    REQUIRE(s.use(e));
    ran(s, "3ka");				// :k leaves the current line
    CHECK(s.current_line() == 5u);
    ran(s, "3");
    ran(s, "ma b");				// :mark on the current line (3)
    ran(s, "4mark c");
    CHECK(ran(s, "'ap") == "three\n");
    ran(s, "1d");
    CHECK(ran(s, "'ap") == "three\n");		// the mark followed its line
    CHECK(ran(s, "'a") == "three\n");
    CHECK(ran(s, "'a,'cp") == "three\nfour\n");
    CHECK(ran(s, "marks")
	  == "mark line  text\n a     2  three\n b     2  three\n"
	     " c     3  four\n");
    CHECK(refused_run(s, "'zp") == "mark not set: 'z");
}

TEST_CASE("ex session: a, i and c take their text in one group")
{
    world w;
    entity_id e = open_doc(w, FIVE);
    session s(w);
    REQUIRE(s.use(e));
    outcome o = s.run("2a");
    CHECK(o.ok);
    CHECK(o.wants_text);
    CHECK(s.wants_text());
    CHECK_FALSE(s.run("p").ok);			// text is pending
    o = s.text({ "x", "y" });
    CHECK(o.ok);
    CHECK_FALSE(s.wants_text());
    CHECK(body(w, e) == "one\ntwo\nx\ny\nthree\nfour\nfive\n");
    CHECK(s.current_line() == 4u);
    s.run("1i");
    s.text({ "zero" });
    CHECK(body(w, e) == "zero\none\ntwo\nx\ny\nthree\nfour\nfive\n");
    CHECK(s.current_line() == 1u);
    s.run("4,5c");
    s.text({ "X" });
    CHECK(body(w, e) == "zero\none\ntwo\nX\nthree\nfour\nfive\n");
    CHECK(s.current_line() == 4u);
    ran(s, "u");
    CHECK(body(w, e) == "zero\none\ntwo\nx\ny\nthree\nfour\nfive\n");
    s.run("$a");
    s.text({});					// no lines: nothing changes
    CHECK(body(w, e) == "zero\none\ntwo\nx\ny\nthree\nfour\nfive\n");
    CHECK_FALSE(s.text({ "late" }).ok);		// nothing is waiting

    world w2;
    entity_id t = open_doc(w2, "");
    session s2(w2);
    REQUIRE(s2.use(t));
    CHECK(s2.current_line() == 0u);
    s2.run("a");				// an empty buffer: after line 0
    s2.text({ "first" });
    CHECK(body(w2, t) == "first\n");
    s2.run("0i");
    s2.text({ "top" });
    CHECK(body(w2, t) == "top\nfirst\n");

    world w3;
    entity_id u = open_doc(w3, "a");		// unterminated
    session s3(w3);
    REQUIRE(s3.use(u));
    s3.run("$a");
    s3.text({ "b" });
    CHECK(body(w3, u) == "a\nb");
}

// ------------------------------------------------- search and substitute
// Slice 3. ORACLE: vim 9.1 `vim -u NONE -i NONE -N -e -s -S cmds file` on
// the same buffer, writing the buffer and line('.') after the commands
// (vim's patterns spelled very-magic `\v` there, PCRE2 here — decision 6).

static const char *ALPHA = "alpha one\nbeta two\nalpha three\ngamma four\n";

// A fresh session on `text`, its current line the last (as vim's).
struct ex_buffer
{
    world w;
    entity_id e;
    session s;
    explicit ex_buffer(const char *text) : e(open_doc(w, text)), s(w)
    {
	REQUIRE(s.use(e));
    }
    std::string text() { return body(w, e); }
};

TEST_CASE("ex session: :s — first or every match, groups, &, line breaks")
{
    {
	ex_buffer b(ALPHA);				// vim: %s/alpha/A/
	ran(b.s, "%s/alpha/A/");
	CHECK(b.text() == "A one\nbeta two\nA three\ngamma four\n");
	CHECK(b.s.current_line() == 3u);
    }
    {
	ex_buffer b(ALPHA);				// vim: 2, s/o/0/g
	ran(b.s, "2");
	ran(b.s, "s/o/0/g");
	CHECK(b.text() == "alpha one\nbeta tw0\nalpha three\ngamma four\n");
	CHECK(b.s.current_line() == 2u);
    }
    {
	ex_buffer b(ALPHA);		// vim: %s/\v(\w+) (\w+)/\2 \1/
	ran(b.s, "%s/(\\w+) (\\w+)/\\2 \\1/");
	CHECK(b.text() == "one alpha\ntwo beta\nthree alpha\nfour gamma\n");
	CHECK(b.s.current_line() == 4u);
    }
    {
	ex_buffer b(ALPHA);				// vim: 3, s/a/[&]/g
	ran(b.s, "3");
	ran(b.s, "s/a/[&]/g");
	CHECK(b.text() == "alpha one\nbeta two\n[a]lph[a] three\ngamma four\n");
    }
    {
	ex_buffer b(ALPHA);				// vim: 1s/ /\r/
	ran(b.s, "1s/ /\\r/");
	CHECK(b.text() == "alpha\none\nbeta two\nalpha three\ngamma four\n");
	CHECK(b.s.current_line() == 2u);
	ran(b.s, "1s/l/\\n/");				// sed's spelling, alike
	CHECK(b.text() == "a\npha\none\nbeta two\nalpha three\ngamma four\n");
    }
    {
	ex_buffer b(ALPHA);				// vim: %s/ALPHA/Z/i
	ran(b.s, "%s/ALPHA/Z/i");
	CHECK(b.text() == "Z one\nbeta two\nZ three\ngamma four\n");
	CHECK(refused_run(b.s, "%s/ALPHA/Z/") == "pattern not found: ALPHA");
	ran(b.s, "%s/ALPHA/Z/e");			// e: no match, no error
	CHECK(refused_run(b.s, "s/(/x/").find("invalid pattern '('") == 0);
    }
    {
	ex_buffer b(ALPHA);				// vim: %s/a//gn
	CHECK(ran(b.s, "%s/a//gn") == "7 matches on 4 lines\n");
	CHECK(b.text() == ALPHA);			// n: counted only
	CHECK(ran(b.s, "1s/a//n") == "1 match on 1 line\n");
    }
    {
	ex_buffer b(ALPHA);
	ran(b.s, "1s/x/y/e");
	ran(b.s, "1d");
	ran(b.s, "u");				// one undo group per :s
	ran(b.s, "%s/a/b/g");
	ran(b.s, "u");
	CHECK(b.text() == ALPHA);
    }
}

TEST_CASE("ex session: :& and :&& repeat the last :s; // is the last pattern")
{
    {
	ex_buffer b(ALPHA);			// vim: 1s/a/X/g, 3&, 4&&
	CHECK(refused_run(b.s, "&") == "no previous substitute");
	ran(b.s, "1s/a/X/g");
	ran(b.s, "3&");				// & drops the flags,
	ran(b.s, "4&&");			// so && keeps none
	CHECK(b.text() == "XlphX one\nbeta two\nXlpha three\ngXmma four\n");
	CHECK(b.s.current_line() == 4u);
	ran(b.s, "2s");				// a bare :s is :&
	CHECK(b.text() == "XlphX one\nbetX two\nXlpha three\ngXmma four\n");
    }
    {
	ex_buffer b(ALPHA);			// vim: 1s/a/X/, 3&g, 4&&
	ran(b.s, "1s/a/X/");
	ran(b.s, "3&g");
	ran(b.s, "4&&");			// && keeps the g & took
	CHECK(b.text() == "Xlpha one\nbeta two\nXlphX three\ngXmmX four\n");
    }
    {
	ex_buffer b(ALPHA);			// vim: 1s/a/X/gi, 3&&
	ran(b.s, "1s/a/X/gi");
	ran(b.s, "3&&");
	CHECK(b.text() == "XlphX one\nbeta two\nXlphX three\ngamma four\n");
	CHECK(b.s.current_line() == 3u);
    }
    {
	ex_buffer b(ALPHA);
	ran(b.s, "/three/");
	ran(b.s, "s//3/");			// the search's pattern
	CHECK(b.text() == "alpha one\nbeta two\nalpha 3\ngamma four\n");
	ran(b.s, "/two/s//2/");			// the range's search first
	CHECK(b.text() == "alpha one\nbeta 2\nalpha 3\ngamma four\n");
    }
}

TEST_CASE("ex session: :g and :v run a command on each marked line")
{
    {
	ex_buffer b(ALPHA);				// vim: g/alpha/d
	ran(b.s, "g/alpha/d");
	CHECK(b.text() == "beta two\ngamma four\n");
	CHECK(b.s.current_line() == 2u);
	ran(b.s, "u");				// one group for every d
	CHECK(b.text() == ALPHA);
    }
    {
	ex_buffer b(ALPHA);				// vim: v/alpha/d
	ran(b.s, "v/alpha/d");
	CHECK(b.text() == "alpha one\nalpha three\n");
	CHECK(b.s.current_line() == 2u);
    }
    {
	ex_buffer b(ALPHA);			// vim: g!/alpha/s/$/!/
	ran(b.s, "g!/alpha/s/$/!/");
	CHECK(b.text() == "alpha one\nbeta two!\nalpha three\ngamma four!\n");
	CHECK(b.s.current_line() == 4u);
    }
    {
	ex_buffer b(ALPHA);				// vim: g/a/s/a/@/g
	ran(b.s, "g/a/s/a/@/g");
	CHECK(b.text() == "@lph@ one\nbet@ two\n@lph@ three\ng@mm@ four\n");
	CHECK(b.s.current_line() == 4u);
	// :s under :g: a line without a match is no error; none at all is.
	CHECK(refused_run(b.s, "g/@/s/q/r/") == "pattern not found: q");
	// No g: the first match on each line. vim says "4 substitutions on 4
	// lines" here (its :g reports n as substitutions); nothing was
	// substituted, so madc says matches, as vim's own :s n does.
	CHECK(ran(b.s, "g/@/s/@//n") == "4 matches on 4 lines\n");
	CHECK(ran(b.s, "g/@/s/@//gn") == "7 matches on 4 lines\n");
    }
    {
	ex_buffer b(ALPHA);				// vim: g/^/m0
	ran(b.s, "g/^/m0");
	CHECK(b.text() == "gamma four\nalpha three\nbeta two\nalpha one\n");
	CHECK(b.s.current_line() == 1u);
    }
    {
	ex_buffer b("a\n\n\n\nb\n\nc\n");		// vim: g/^$/,/./-j
	ran(b.s, "g/^$/,/./-j");
	CHECK(b.text() == "a\n\nb\n\nc\n");
	CHECK(b.s.current_line() == 4u);
    }
    {
	ex_buffer b("x1\nx2\nx3\ny\n");			// vim: g/x/.,+1j
	ran(b.s, "g/x/.,+1j");				// x2: joined away
	CHECK(b.text() == "x1 x2\nx3 y\n");
	CHECK(b.s.current_line() == 2u);
    }
    {
	ex_buffer b(ALPHA);
	CHECK(ran(b.s, "g/alpha/") == "alpha one\nalpha three\n");	// p
	CHECK(ran(b.s, "g/zzz/p") == "pattern not found: zzz\n");
	CHECK(refused_run(b.s, "g/a/g/b/p") == "cannot nest :global");
	CHECK(refused_run(b.s, "g/a/a") == "append cannot run under :global");
	CHECK(refused_run(b.s, "g/a/u") == "undo cannot run under :global");
    }
}

static const char *SORTME = "b10\na2\nB1\na2\nc-3\nx0x1F\n";

static std::string sorted(const char *cmd)
{
    ex_buffer b(SORTME);
    ran(b.s, cmd);
    CHECK(b.s.current_line() == 1u);		// vim: the range's first
    return b.text();
}

TEST_CASE("ex session: :sort — text, numbers, case, unique, a pattern, reversed")
{
    // vim: sort, sort!, sort i, sort u, sort n, sort x, sort! n
    CHECK(sorted("sort") == "B1\na2\na2\nb10\nc-3\nx0x1F\n");
    CHECK(sorted("sort!") == "x0x1F\nc-3\nb10\na2\na2\nB1\n");
    CHECK(sorted("sor i") == "a2\na2\nB1\nb10\nc-3\nx0x1F\n");
    CHECK(sorted("sort u") == "B1\na2\nb10\nc-3\nx0x1F\n");
    CHECK(sorted("sort n") == "c-3\nx0x1F\nB1\na2\na2\nb10\n");
    CHECK(sorted("sort x") == "c-3\nx0x1F\na2\na2\nB1\nb10\n");
    CHECK(sorted("sort! n") == "b10\na2\na2\nB1\nx0x1F\nc-3\n");
    // vim: sort /\a/ (after the first letter), sort /\d\+/ r (the match)
    CHECK(sorted("sort /[A-Za-z]/") == "c-3\nx0x1F\nB1\nb10\na2\na2\n");
    CHECK(sorted("sort /\\d+/ r") == "x0x1F\nB1\nb10\na2\na2\nc-3\n");
    {
	ex_buffer b("b\nA\na\nB\na\n");			// vim: sort iu
	ran(b.s, "sort iu");
	CHECK(b.text() == "A\nb\n");
    }
    {
	ex_buffer b(SORTME);
	ran(b.s, "2,4sort");				// a range
	CHECK(b.text() == "b10\nB1\na2\na2\nc-3\nx0x1F\n");
	CHECK(b.s.current_line() == 2u);
	ran(b.s, "u");
	CHECK(b.text() == SORTME);
    }
    {
	ex_buffer b("1\n2\n3");				// unterminated stays so
	ran(b.s, "sort!");
	CHECK(b.text() == "3\n2\n1");
    }
    ex_buffer b(SORTME);
    CHECK(refused_run(b.s, "sort nx") == "sort takes one of n x o b f");
    CHECK(refused_run(b.s, "sort q") == "invalid sort argument: 'q'");
}

static const char *TABBED = "\tone\n    two\n  \t three\nx\ty  z\n";

TEST_CASE("ex session: :retab — runs holding a tab, ! for spaces, expandtab")
{
    {
	ex_buffer b(TABBED);				// vim: retab 4
	ran(b.s, "retab 4");
	CHECK(b.text() == "\t\tone\n    two\n\t\t three\nx\t\ty  z\n");
	CHECK(b.s.settings().tabstop == 4u);
    }
    {
	ex_buffer b(TABBED);				// vim: set et, retab
	b.s.settings().expandtab = true;
	ran(b.s, "ret");
	CHECK(b.text()
	      == "        one\n    two\n         three\nx       y  z\n");
    }
    {
	ex_buffer b(TABBED);				// vim: retab! 4
	ran(b.s, "retab! 4");
	CHECK(b.text() == "\t\tone\n\ttwo\n\t\t three\nx\t\ty  z\n");
    }
    {
	ex_buffer b(TABBED);			// vim: set ts=4, retab! 8
	b.s.settings().tabstop = 4;
	ran(b.s, "retab! 8");
	CHECK(b.text() == "    one\n    two\n     three\nx   y  z\n");
	CHECK(b.s.settings().tabstop == 8u);
    }
    ex_buffer b(TABBED);
    CHECK(refused_run(b.s, "retab x") == "trailing characters: 'x'");
}

// ------------------------------------------------------ files and buffers
// Slice 4. ORACLE: vim 9.1 `vim -u NONE -i NONE -N -e -s -S` with :redir,
// in a directory holding a.txt ("one\ntwo\nthree\n") and b.txt ("x\ny\n"):
// e a.txt, e b.txt, e new.txt, ls, e #, ls, w c.txt, 2w d.txt, w >> d.txt,
// 1,2w c.txt, r b.txt, pwd, 2, q, sav e.txt, ls. One divergence: vim's
// :sav keeps a buffer for the old name as the alternate; madc renames the
// one buffer and the alternate stays.

#include <cstdio>
#include <fstream>
#include <iterator>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// A directory the test makes under the current one and removes, with
// every file named in it.
struct scratch_dir
{
    std::string name;
    std::vector<std::string> files;
    scratch_dir() : name("test_ex_files." + std::to_string((long)getpid()))
    {
#ifdef _WIN32
	REQUIRE(_mkdir(name.c_str()) == 0);
#else
	REQUIRE(mkdir(name.c_str(), 0700) == 0);
#endif
    }
    void put(const std::string &file, const std::string &text)
    {
	std::ofstream(path(file).c_str(), std::ios::binary) << text;
	files.push_back(file);
    }
    std::string path(const std::string &file) const { return name + "/" + file; }
    std::string read(const std::string &file) const
    {
	std::ifstream in(path(file).c_str(), std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)),
			   std::istreambuf_iterator<char>());
    }
    ~scratch_dir()
    {
	for ( size_t i = 0; i < files.size(); ++i )
	    remove(path(files[i]).c_str());
#ifdef _WIN32
	_rmdir(name.c_str());
#else
	rmdir(name.c_str());
#endif
    }
};

static std::string ran(session &s, const std::string &cmd)
{
    return ran(s, cmd.c_str());
}

TEST_CASE("ex session: :e :r :w :sav, :ls, :q — vim's messages")
{
    scratch_dir d;
    d.put("a.txt", "one\ntwo\nthree\n");
    d.put("b.txt", "x\ny\n");
    d.files.push_back("c.txt");
    d.files.push_back("d.txt");
    d.files.push_back("e.txt");
    world w;
    session s(w);
    CHECK(ran(s, "cd " + d.name) == "");
    CHECK(ran(s, "e a.txt") == "\"a.txt\" 3L, 14B\n");
    CHECK(s.current_line() == 3u);
    CHECK(ran(s, "e b.txt") == "\"b.txt\" 2L, 4B\n");
    CHECK(ran(s, "e new.txt") == "\"new.txt\" [New]\n");
    CHECK(ran(s, "ls")
	  == "  1      \"a.txt\"                        line 3\n"
	     "  2 #    \"b.txt\"                        line 2\n"
	     "  3 %a   \"new.txt\"                      line 1\n");
    CHECK(ran(s, "e #") == "\"b.txt\" 2L, 4B\n");
    CHECK(ran(s, "ls")
	  == "  1      \"a.txt\"                        line 3\n"
	     "  2 %a   \"b.txt\"                        line 2\n"
	     "  3 #    \"new.txt\"                      line 1\n");
    CHECK(ran(s, "w c.txt") == "\"c.txt\" [New] 2L, 4B written\n");
    CHECK(ran(s, "2w d.txt") == "\"d.txt\" [New] 1L, 2B written\n");
    CHECK(ran(s, "w >> d.txt") == "\"d.txt\" 2L, 4B appended\n");
    CHECK(refused_run(s, "1,2w c.txt") == "file exists (add ! to override)");
    CHECK(ran(s, "r b.txt") == "\"b.txt\" 2L, 4B\n");
    CHECK(s.current_line() == 4u);
    CHECK(ran(s, "2") == "y\n");
    CHECK(refused_run(s, "q")
	  == "no write since last change (add ! to override)");
    CHECK(refused_run(s, "1w") == "use ! to write a partial buffer");
    CHECK(ran(s, "sav e.txt") == "\"e.txt\" [New] 4L, 8B written\n");
    CHECK(ran(s, "ls")
	  == "  1      \"a.txt\"                        line 3\n"
	     "  2 %a   \"e.txt\"                        line 2\n"
	     "  3 #    \"new.txt\"                      line 1\n");
    CHECK(d.read("c.txt") == "x\ny\n");
    CHECK(d.read("d.txt") == "y\nx\ny\n");
    CHECK(d.read("e.txt") == "x\ny\nx\ny\n");
    CHECK(d.read("b.txt") == "x\ny\n");		// :sav leaves the old file
    outcome o = s.run("q");
    CHECK(o.ok);
    CHECK(o.quit);
}

TEST_CASE("ex session: :w writes the buffer and its state; :x and :up only changes")
{
    scratch_dir d;
    d.put("f.txt", "1\n2\n");
    world w;
    session s(w);
    ran(s, "cd " + d.name);
    ran(s, "e f.txt");
    CHECK(ran(s, "up") == "");			// nothing changed
    ran(s, "1d");
    CHECK(refused_run(s, "e") == "no write since last change (add ! to override)");
    CHECK(ran(s, "up") == "\"f.txt\" 1L, 2B written\n");
    CHECK(d.read("f.txt") == "2\n");
    CHECK_FALSE(w.doc_modified(s.current()));
    ran(s, "a");
    s.text(std::vector<std::string>(1, "3"));
    CHECK(ran(s, "e!") == "\"f.txt\" 1L, 2B\n");	// the changes dropped
    CHECK(body(w, s.current()) == "2\n");
    ran(s, "$a");
    s.text(std::vector<std::string>(1, "4"));
    outcome o = s.run("x");
    CHECK(o.ok);
    CHECK(o.quit);
    CHECK(o.shown == "\"f.txt\" 2L, 4B written\n");
    CHECK(d.read("f.txt") == "2\n4\n");

    world w2;
    session u(w2);
    ran(u, "cd " + d.name);
    ran(u, "e untitled.txt");			// a new buffer: nothing written
    CHECK(refused_run(u, "w nested/x.txt") == "cannot write \"nested/x.txt\"");
    outcome q = u.run("q");
    CHECK(q.quit);
}

TEST_CASE("ex session: :b :bn :bp switch buffers; :cd and :pwd are the session's")
{
    scratch_dir d;
    d.put("one.c", "1\n");
    d.put("two.c", "2\n");
    d.put("three.h", "3\n");
    world w;
    session s(w);
    CHECK(refused_run(s, "p") == "no buffer (:e FILE opens one)");
    CHECK(refused_run(s, "cd no-such-dir") == "cannot change directory: no-such-dir");
    const std::string before = s.directory();
    ran(s, "cd " + d.name);
    CHECK(ran(s, "pwd") == s.directory() + "\n");
    CHECK(s.directory() != before);
    ran(s, "e one.c");
    ran(s, "e two.c");
    ran(s, "e three.h");
    ran(s, "b 1");
    CHECK(body(w, s.current()) == "1\n");
    ran(s, "bn");
    CHECK(body(w, s.current()) == "2\n");
    ran(s, "bp");
    ran(s, "bp");				// wraps to the last
    CHECK(body(w, s.current()) == "3\n");
    ran(s, "b two");
    CHECK(body(w, s.current()) == "2\n");
    CHECK(refused_run(s, "b .c") == "more than one match for .c");
    CHECK(refused_run(s, "b 9") == "buffer 9 does not exist");
    ran(s, "b #");
    CHECK(body(w, s.current()) == "3\n");
    ran(s, "cd -");
    CHECK(s.directory() == before);
    ran(s, "cd -");
    CHECK(ran(s, "e one.c") == "\"one.c\" 1L, 2B\n");	// the same buffer
    CHECK(w.docs().size() == 3u);
}

TEST_CASE("ex session: :set — the four options in vim's forms")
{
    world w;
    session s(w);
    CHECK(ran(s, "set") == "--- Options ---\n");
    CHECK(ran(s, "set ts=4 et") == "");
    CHECK(ran(s, "set") == "--- Options ---\n  expandtab\n  tabstop=4\n");
    CHECK(ran(s, "se ts?") == "  tabstop=4\n");
    CHECK(ran(s, "set nu?") == "nonumber\n");
    ran(s, "set invnu");
    CHECK(ran(s, "set nu?") == "  number\n");
    ran(s, "set nu!");
    CHECK_FALSE(s.settings().number);
    CHECK(ran(s, "set sw") == "  shiftwidth=8\n");
    ran(s, "set sw:2 noet");
    CHECK(s.settings().shiftwidth == 2u);
    CHECK_FALSE(s.settings().expandtab);
    ran(s, "set ts&");
    CHECK(s.settings().tabstop == 8u);
    CHECK(ran(s, "set all")
	  == "--- Options ---\nnoexpandtab\nnonumber\n  shiftwidth=2\n"
	     "  tabstop=8\n");
    CHECK(refused_run(s, "set foo") == "unknown option: foo");
    CHECK(refused_run(s, "set ts=0") == "argument must be positive: ts=0");
    CHECK(refused_run(s, "set nu=3") == "invalid argument: nu=3");
}

#ifndef _WIN32
TEST_CASE("ex session: :!cmd, :r !cmd and :w !cmd through the shell")
{
    scratch_dir d;
    d.put("s.txt", "b\na\n");
    world w;
    session s(w);
    ran(s, "cd " + d.name);
    CHECK(ran(s, "!echo hi") == "hi\n");
    CHECK(ran(s, "!pwd") == s.directory() + "\n");	// the session's directory
    CHECK(ran(s, "!exit 3") == "shell returned 3\n");
    CHECK(refused_run(s, "!echo %") == "no file name for '%'");
    ran(s, "e s.txt");
    CHECK(ran(s, "!echo % \\%") == "s.txt %\n");
    CHECK(ran(s, "w !sort") == "a\nb\n");
    CHECK(ran(s, "2w !cat") == "a\n");
    CHECK(ran(s, "w !head -1") == "b\n");	// stops reading: no error
    ran(s, "0r !printf 'x\\ny\\n'");
    CHECK(body(w, s.current()) == "x\ny\nb\na\n");
    CHECK(s.current_line() == 2u);
}
#endif
