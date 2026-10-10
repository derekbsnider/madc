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
