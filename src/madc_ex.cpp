// madc_ex.cpp — the ex core (split arc Phase 1B.6; design
// docs/plans/2026-10-10-ex-core-design.md; header include/madc_ex.h).
//
// The command line's grammar is vim's ex, line-oriented (D24): addresses
// and ranges, a command name resolved once through ONE table to its code,
// and each command's operands. Oracle: vim's ex mode (`vim -e -s`) on the
// same line.
//
// THREAD-SAFETY CONTRACT: see madc_ex.h — the grammar is pure functions
// over their input and one immutable table; a session is confined to its
// world's thread.

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "madc_documents.h"	// :e :r :w :sav — the document file owner
#include "madc_ex.h"
#include "madc_ex_session.h"
#include "ns_common.h"		// madc_ui_world: a ui:: world handle's world
#include "madc_file_kinds.h"	// a buffer's indent: indent_detect, the kind's default
#include "madc_posix_io.h"	// host_path_join / _type_of / _within: paths
#include "madc_regex.h"		// /re/ ?re? s g v sor — madc's one regex engine
#include "madcdis/hub.h"
#include "madcdis/process.h"	// :! :r ! :w ! — the one spawn owner
#include "madcdis/verbs.h"	// mutation_context: the one write surface

namespace madc {
namespace ex {

namespace {

// The command table: a typed name selects the row whose full name it
// begins, when it is at least `shortest` bytes long (vim's abbreviation
// rule: `s` is substitute, `sor` is sort, `sa` is nothing). The rows are
// chosen so that no typed name selects two.
struct command_row
{
    const char   *name;
    unsigned char shortest;
    command       code;
};

const command_row command_rows[] = {
    { "append",     1, command::append },
    { "bNext",      2, command::bprevious },
    { "bnext",      2, command::bnext },
    { "bprevious",  2, command::bprevious },
    { "buffer",     1, command::buffer },
    { "buffers",    7, command::buffers },
    { "cd",         2, command::cd },
    { "change",     1, command::change },
    { "copy",       2, command::copy },
    { "delete",     1, command::del },
    { "edit",       1, command::edit },
    { "exit",       3, command::exit },
    { "files",      5, command::buffers },
    { "global",     1, command::global },
    { "help",       1, command::help },
    { "insert",     1, command::insert },
    { "join",       1, command::join },
    { "k",          1, command::mark },
    { "list",       1, command::list },
    { "ls",         2, command::buffers },
    { "mark",       2, command::mark },
    { "marks",      5, command::marks },
    { "move",       1, command::move },
    { "number",     1, command::number },
    { "print",      1, command::print },
    { "pwd",        2, command::pwd },
    { "quit",       1, command::quit },
    { "read",       1, command::read },
    { "redo",       3, command::redo },
    { "retab",      3, command::retab },
    { "saveas",     3, command::saveas },
    { "set",        2, command::set },
    { "sort",       3, command::sort },
    { "substitute", 1, command::substitute },
    { "t",          1, command::copy },
    { "undo",       1, command::undo },
    { "update",     2, command::update },
    { "vglobal",    1, command::vglobal },
    { "wq",         2, command::write_quit },
    { "write",      1, command::write },
    { "xit",        1, command::exit },
};

// The command's own name, for messages: the full name of its first row,
// or the symbol it is typed as.
struct symbol_name
{
    command     code;
    const char *name;
};

const symbol_name symbol_names[] = {
    { command::line_number,      "=" },
    { command::shift_right,      ">" },
    { command::shift_left,       "<" },
    { command::repeat_sub,       "&" },
    { command::repeat_sub_flags, "&&" },
    { command::shell,            "!" },
    { command::none,             "" },
};

bool blank(char c)
{
    return c == ' ' || c == '\t';
}

// The reader over one command line: `at` is the next byte.
struct reader
{
    const std::string &s;
    size_t at;

    explicit reader(const std::string &text) : s(text), at(0) {}

    bool end() const { return at >= s.size(); }
    char peek() const { return end() ? '\0' : s[at]; }
    void skip_blanks()
    {
	while ( !end() && blank(s[at]) )
	    ++at;
    }
    bool digits(size_t &n)
    {
	if ( end() || !isdigit((unsigned char)s[at]) )
	    return false;
	n = 0;
	while ( !end() && isdigit((unsigned char)s[at]) )
	    n = n * 10 + (size_t)(s[at++] - '0');
	return true;
    }
    // The text up to the next unescaped `delim` (consumed), or to the end.
    // A backslash keeps its escape: the pattern goes to PCRE2 as typed,
    // where `\/` and `\?` are literals; `strip` (a replacement) drops the
    // backslash before the delimiter only.
    std::string delimited(char delim, bool strip)
    {
	std::string out;
	while ( !end() && s[at] != delim )
	{
	    if ( s[at] == '\\' && at + 1 < s.size() )
	    {
		if ( !(strip && s[at + 1] == delim) )
		    out += '\\';
		out += s[at + 1];
		at += 2;
		continue;
	    }
	    out += s[at++];
	}
	if ( !end() )
	    ++at;		// the closing delimiter
	return out;
    }
    std::string rest()
    {
	std::string r = s.substr(at < s.size() ? at : s.size());
	at = s.size();
	while ( !r.empty() && blank(r[r.size() - 1]) )
	    r.erase(r.size() - 1);
	return r;
    }
};

// The offsets after an address's base (or alone): `+N`, `-N`, a bare `+` /
// `-` for 1, and digits right after a base, which add (vim's `.5`).
void read_offsets(reader &r, address &a, bool after_base)
{
    for ( ;; )
    {
	char c = r.peek();
	if ( c == '+' || c == '-' )
	{
	    ++r.at;
	    size_t n = 1;
	    r.digits(n);
	    a.offset += c == '+' ? (long)n : -(long)n;
	    continue;
	}
	size_t n = 0;
	if ( after_base && r.digits(n) )
	{
	    a.offset += (long)n;
	    continue;
	}
	return;
    }
}

// One address, or nothing (false: no address starts here). A mark that is
// no letter, or a search that never closes its pattern, is still read; the
// session refuses what it cannot resolve.
bool read_address(reader &r, address &a, std::string &error)
{
    a = address();
    char c = r.peek();
    if ( isdigit((unsigned char)c) )
    {
	a.kind = address::base::line;
	r.digits(a.line);
    }
    else if ( c == '.' )
    {
	a.kind = address::base::current;
	++r.at;
    }
    else if ( c == '$' )
    {
	a.kind = address::base::last;
	++r.at;
    }
    else if ( c == '\'' )
    {
	++r.at;
	char m = r.peek();
	if ( !(m >= 'a' && m <= 'z') )
	{
	    error = "invalid mark: a mark is a letter a-z";
	    return false;
	}
	a.kind = address::base::mark;
	a.mark_name = m;
	++r.at;
    }
    else if ( c == '/' || c == '?' )
    {
	++r.at;
	a.kind = c == '/' ? address::base::search_forward
			  : address::base::search_backward;
	a.pattern = r.delimited(c, false);
    }
    else if ( c == '\\' && r.at + 1 < r.s.size()
	      && (r.s[r.at + 1] == '/' || r.s[r.at + 1] == '?'
		  || r.s[r.at + 1] == '&') )
    {
	// \/ \? \&: the last pattern, searched forward / backward.
	a.kind = r.s[r.at + 1] == '?' ? address::base::search_backward
				      : address::base::search_forward;
	r.at += 2;
    }
    else if ( c == '+' || c == '-' )
    {
	read_offsets(r, a, false);
	return true;
    }
    else
	return false;
    read_offsets(r, a, true);
    return true;
}

// The range before the name: `%`, or addresses separated by `,` / `;` (a
// missing one beside a separator is the current line; of more than two, the
// last two count, as in vim).
bool read_range(reader &r, range &out, std::string &error)
{
    out = range();
    r.skip_blanks();
    if ( r.peek() == '%' )
    {
	++r.at;
	out.count = 2;
	out.first.kind = address::base::line;
	out.first.line = 1;
	out.second.kind = address::base::last;
	read_offsets(r, out.second, false);
	return true;
    }
    address a;
    std::string why;
    bool have = read_address(r, a, why);
    if ( !why.empty() )
    {
	error = why;
	return false;
    }
    if ( have )
    {
	out.count = 1;
	out.second = a;
    }
    for ( ;; )
    {
	r.skip_blanks();
	char sep = r.peek();
	if ( sep != ',' && sep != ';' )
	    break;
	++r.at;
	if ( !have )
	{
	    out.second = address();
	    out.second.kind = address::base::current;
	}
	out.first = out.second;
	out.from_current = sep == ';';
	r.skip_blanks();
	have = read_address(r, a, why);
	if ( !why.empty() )
	{
	    error = why;
	    return false;
	}
	if ( !have )
	{
	    a = address();
	    a.kind = address::base::current;
	    have = true;
	}
	out.second = a;
	out.count = 2;
    }
    if ( out.count == 1 )
	out.first = out.second;
    return true;
}

// A trailing count (`:d 3`, `:>> 2`); nothing else may follow.
bool read_count(reader &r, line &out, std::string &error)
{
    r.skip_blanks();
    size_t n = 0;
    if ( r.digits(n) )
    {
	if ( n == 0 )
	{
	    error = "a count must be positive";
	    return false;
	}
	out.has_count = true;
	out.count = n;
    }
    r.skip_blanks();
    if ( !r.end() )
    {
	error = "trailing characters: '" + r.rest() + "'";
	return false;
    }
    return true;
}

// `s`'s pattern, replacement, flags and count, after the name. No
// delimiter: `:s [flags] [count]` repeats the last substitute.
bool read_substitute(reader &r, line &out, std::string &error)
{
    char d = r.peek();
    bool delimiter = d && !isalnum((unsigned char)d) && !blank(d)
		     && d != '"' && d != '|' && d != '\\';
    if ( !delimiter )
    {
	out.code = command::repeat_sub;
	r.skip_blanks();
    }
    else
    {
	++r.at;
	out.pattern = r.delimited(d, false);
	out.replacement = r.delimited(d, true);
    }
    while ( !r.end() && strchr("&cegiIn", r.peek()) )
    {
	if ( r.peek() == 'c' )
	{
	    error = "the c flag asks before each change, which needs normal"
		    " mode";
	    return false;
	}
	out.flags += r.s[r.at++];
    }
    return read_count(r, out, error);
}

// `g/re/cmd` and `v/re/cmd`: the pattern, then the command each matching
// line runs (none: `p`).
bool read_global(reader &r, line &out, std::string &error)
{
    r.skip_blanks();
    char d = r.peek();
    if ( !d || isalnum((unsigned char)d) || d == '"' || d == '|'
	 || d == '\\' )
    {
	error = std::string(command_name(out.code))
	      + " needs a pattern: " + command_name(out.code) + "/re/cmd";
	return false;
    }
    ++r.at;
    out.pattern = r.delimited(d, false);
    out.subcommand = r.rest();
    size_t lead = 0;
    while ( lead < out.subcommand.size() && blank(out.subcommand[lead]) )
	++lead;
    out.subcommand.erase(0, lead);
    return true;
}

// `sor`'s options and pattern, in any order (vim's): `i` `u` `r`, one of
// `n x o b f`, and a pattern between any non-letter delimiters.
bool read_sort(reader &r, line &out, std::string &error)
{
    for ( ;; )
    {
	r.skip_blanks();
	if ( r.end() )
	    return true;
	const char c = r.peek();
	if ( strchr("iur", c) )
	    out.flags += r.s[r.at++];
	else if ( strchr("nxobf", c) )
	{
	    for ( char f : out.flags )
		if ( strchr("nxobf", f) && f != c )
		{
		    error = "sort takes one of n x o b f";
		    return false;
		}
	    out.flags += r.s[r.at++];
	}
	else if ( !isalnum((unsigned char)c) && c != '"' && c != '\\'
		  && c != '|' )
	{
	    ++r.at;
	    out.pattern = r.delimited(c, false);
	    out.has_pattern = true;
	}
	else
	{
	    error = "invalid sort argument: '" + r.rest() + "'";
	    return false;
	}
    }
}

// `ret`'s new tabstop: digits, or none (0 — the current one, vim's).
bool read_tabstop(reader &r, line &out, std::string &error)
{
    r.skip_blanks();
    size_t n = 0;
    if ( r.digits(n) )
	out.count = n;
    r.skip_blanks();
    if ( !r.end() )
    {
	error = "trailing characters: '" + r.rest() + "'";
	return false;
    }
    return true;
}

} // namespace

const char *command_name(command c)
{
    for ( const symbol_name &s : symbol_names )
	if ( s.code == c && *s.name )
	    return s.name;
    for ( const command_row &row : command_rows )
	if ( row.code == c )
	    return row.name;
    return "";
}

bool command_of(const std::string &name, command &code)
{
    for ( const command_row &row : command_rows )
	if ( name.size() >= row.shortest
	     && strncmp(row.name, name.c_str(), name.size()) == 0
	     && name.size() <= strlen(row.name) )
	{
	    code = row.code;
	    return true;
	}
    return false;
}

bool parse(const std::string &text, line &out, std::string &error)
{
    out = line();
    error.clear();
    reader r(text);
    // Extra colons and blanks before the range, as vim skips them.
    while ( !r.end() && (r.peek() == ':' || blank(r.peek())) )
	++r.at;
    if ( !read_range(r, out.where, error) )
	return false;
    r.skip_blanks();
    if ( r.end() )
	return true;			// a bare address, or nothing
    char c = r.peek();
    if ( isalpha((unsigned char)c) )
    {
	size_t start = r.at;
	while ( !r.end() && isalpha((unsigned char)r.peek()) )
	    ++r.at;
	std::string name = text.substr(start, r.at - start);
	if ( !command_of(name, out.code) )
	{
	    // `:ka` — mark a, the name and its letter run together (vim).
	    if ( name.size() == 2 && name[0] == 'k' && name[1] >= 'a'
		 && name[1] <= 'z' )
	    {
		out.code = command::mark;
		out.argument = name.substr(1);
		r.skip_blanks();
		if ( !r.end() )
		{
		    error = "trailing characters: '" + r.rest() + "'";
		    return false;
		}
		return true;
	    }
	    error = "not an editor command: '" + text.substr(start) + "'";
	    return false;
	}
	if ( r.peek() == '!' )
	{
	    ++r.at;
	    if ( out.code == command::read )
		out.shell_form = true;	// :r!cmd reads a command's output
	    else if ( out.code == command::global )
		out.code = command::vglobal;	// :g! is :v
	    else
		out.force = true;
	}
    }
    else if ( c == '=' )
    {
	++r.at;
	out.code = command::line_number;
    }
    else if ( c == '#' )
    {
	++r.at;
	out.code = command::number;
    }
    else if ( c == '>' || c == '<' )
    {
	out.code = c == '>' ? command::shift_right : command::shift_left;
	while ( r.peek() == c )
	{
	    ++r.at;
	    ++out.levels;
	}
    }
    else if ( c == '&' )
    {
	++r.at;
	out.code = command::repeat_sub;
	if ( r.peek() == '&' )
	{
	    ++r.at;
	    out.code = command::repeat_sub_flags;
	}
    }
    else if ( c == '!' )
    {
	++r.at;
	out.code = command::shell;
	out.argument = r.rest();
	if ( out.argument.empty() )
	{
	    error = "! needs a command";
	    return false;
	}
	return true;
    }
    else
    {
	error = "not an editor command: '" + text.substr(r.at) + "'";
	return false;
    }

    switch ( out.code )
    {
	case command::print: case command::number: case command::list:
	case command::del: case command::join:
	case command::shift_right: case command::shift_left:
	    return read_count(r, out, error);
	case command::line_number: case command::append:
	case command::insert: case command::change:
	case command::undo: case command::redo:
	case command::write_quit: case command::exit: case command::update:
	case command::quit: case command::pwd: case command::bnext:
	case command::bprevious: case command::buffers:
	    r.skip_blanks();
	    if ( !r.end() )
	    {
		error = "trailing characters: '" + r.rest() + "'";
		return false;
	    }
	    return true;
	case command::substitute:
	    return read_substitute(r, out, error);
	case command::repeat_sub: case command::repeat_sub_flags:
	    while ( !r.end() && strchr("&egiIn", r.peek()) )
		out.flags += r.s[r.at++];
	    return read_count(r, out, error);
	case command::global: case command::vglobal:
	    return read_global(r, out, error);
	case command::sort:
	    return read_sort(r, out, error);
	case command::retab:
	    return read_tabstop(r, out, error);
	case command::move: case command::copy:
	{
	    r.skip_blanks();
	    std::string why;
	    if ( !read_address(r, out.destination, why) )
	    {
		error = why.empty() ? std::string(command_name(out.code))
				      + " needs a destination address"
				    : why;
		return false;
	    }
	    r.skip_blanks();
	    if ( !r.end() )
	    {
		error = "trailing characters: '" + r.rest() + "'";
		return false;
	    }
	    return true;
	}
	case command::mark:
	    r.skip_blanks();
	    if ( r.end() || !(r.peek() >= 'a' && r.peek() <= 'z') )
	    {
		error = "mark needs a letter a-z";
		return false;
	    }
	    out.argument = std::string(1, r.s[r.at++]);
	    r.skip_blanks();
	    if ( !r.end() )
	    {
		error = "trailing characters: '" + r.rest() + "'";
		return false;
	    }
	    return true;
	case command::read: case command::write:
	    r.skip_blanks();
	    if ( out.code == command::write && r.peek() == '>'
		 && r.at + 1 < r.s.size() && r.s[r.at + 1] == '>' )
	    {
		out.flags = ">>";		// :w >> FILE appends
		r.at += 2;
		r.skip_blanks();
	    }
	    if ( !out.shell_form && r.peek() == '!' )
	    {
		++r.at;
		out.shell_form = true;
	    }
	    out.argument = r.rest();
	    if ( out.shell_form )
	    {
		size_t lead = 0;
		while ( lead < out.argument.size()
			&& blank(out.argument[lead]) )
		    ++lead;
		out.argument.erase(0, lead);
		if ( out.argument.empty() )
		{
		    error = "! needs a command";
		    return false;
		}
	    }
	    return true;
	default:
	    // e, sav, cd, b, se, h, marks: the rest as typed.
	    r.skip_blanks();
	    out.argument = r.rest();
	    return true;
    }
}

// ------------------------------------------------------------------ session

namespace {

using madc::hub::anchor;
using madc::hub::anchor_id;
using madc::hub::entity_id;
using madc::hub::mutation_context;
using madc::hub::text_buffer;

bool blank_byte(char c)
{
    return c == ' ' || c == '\t';
}

// The default range a command takes when none is typed (vim's).
enum class default_range : unsigned char
{
    current,		// .
    whole,		// %
    last,		// $
    current_next	// .,.+1 (join)
};

} // namespace

// One command line's execution over a session: the session's state and
// the outcome it fills. A friend of session (madc_ex.h).
struct executor
{
    session &s;
    outcome &out;
    bool checkpointed;		// this command line's undo group is open
    bool under_global;		// running :g's command on one of its lines
    size_t global_matches;	// :s under :g: its matches, every line
    size_t global_lines;	//   and the lines they were on
    // :g's marked lines not yet run, ascending, each stored less `bias` (an
    // edit before every one of them moves them all at once).
    std::deque<long long> global_marks;
    long long global_bias;

    executor(session &ss, outcome &o)
	: s(ss), out(o), checkpointed(false), under_global(false),
	  global_matches(0), global_lines(0), global_bias(0) {}

    // ---- :g's marks follow every edit a command makes (vim keeps the
    // mark on the line itself): a deleted line loses its mark, the lines
    // after an edit move with it.
    void marks_deleted(size_t first, size_t last)
    {
	if ( global_marks.empty() )
	    return;
	const long long gone = (long long)(last - first + 1);
	if ( global_marks.front() + global_bias > (long long)last )
	{
	    global_bias -= gone;
	    return;
	}
	std::deque<long long> kept;
	for ( long long stored : global_marks )
	{
	    const long long at = stored + global_bias;
	    if ( at < (long long)first )
		kept.push_back(stored);
	    else if ( at > (long long)last )
		kept.push_back(stored - gone);
	}
	global_marks.swap(kept);
    }
    void marks_inserted(size_t after, size_t count)
    {
	if ( global_marks.empty() || count == 0 )
	    return;
	if ( global_marks.front() + global_bias > (long long)after )
	{
	    global_bias += (long long)count;
	    return;
	}
	for ( long long &stored : global_marks )
	    if ( stored + global_bias > (long long)after )
		stored += (long long)count;
    }

    bool fail(const std::string &why)
    {
	out.ok = false;
	out.error = why;
	return false;
    }

    // ---- reading the current buffer
    const text_buffer *text() const { return s.w.text_of(s.cur); }
    size_t lines() const
    {
	const text_buffer *b = text();
	return b ? b->line_count() : 0;
    }
    std::string line_text(size_t n) const
    {
	const text_buffer *b = text();
	size_t off = 0, len = 0;
	if ( !b || !b->line_span(n, off, len) )
	    return std::string();
	return b->slice(off, len);
    }
    // The offset line `n` starts at; past the last line, the text's end.
    size_t line_start(size_t n) const
    {
	const text_buffer *b = text();
	size_t off = 0, len = 0;
	if ( b && b->line_span(n, off, len) )
	    return off;
	return b ? b->size() : 0;
    }
    bool unterminated() const
    {
	const text_buffer *b = text();
	return b && b->size() > 0 && b->slice(b->size() - 1, 1) != "\n";
    }

    // ---- the current line
    size_t dot() const { return s.current_line(); }
    void set_dot(size_t n)
    {
	session::buffer_state &st = s.buffers[s.cur];
	mutation_context mc(s.w);
	size_t off = n == 0 ? 0 : line_start(n);
	if ( !st.dot || !mc.anchor_move(s.cur, st.dot, off) )
	    st.dot = mc.anchor_add(s.cur, off, false);
    }

    // ---- addresses
    bool search(const std::string &typed, bool forward, size_t from,
		size_t &found)
    {
	const std::string pattern = typed.empty() ? s.last_pattern : typed;
	if ( pattern.empty() )
	    return fail("no previous pattern");
	std::unique_ptr<madc::regex> re;
	if ( !compile(pattern, false, re) )
	    return false;
	s.last_pattern = pattern;
	const size_t n = lines();
	const size_t base = from == 0 ? (forward ? 0 : n + 1) : from;
	for ( size_t i = 1; i <= n; ++i )
	{
	    // Wrapping, as vim's 'wrapscan': forward from the line after
	    // `from` to `from` itself, backward from the line before it.
	    size_t l = forward ? (base + i - 1) % n + 1
			       : (base - 1 + n - i) % n + 1;
	    if ( re->search(line_text(l)) )
	    {
		found = l;
		return true;
	    }
	}
	return fail("pattern not found: " + pattern);
    }

    bool resolve(const address &a, size_t &result, bool zero_ok)
    {
	const size_t n = lines();
	size_t base = 0;
	switch ( a.kind )
	{
	    case address::base::none:
	    case address::base::current:
		base = dot();
		break;
	    case address::base::line:
		base = a.line;
		break;
	    case address::base::last:
		base = n;
		break;
	    case address::base::search_forward:
	    case address::base::search_backward:
		if ( !search(a.pattern,
			     a.kind == address::base::search_forward, dot(),
			     base) )
		    return false;
		break;
	    case address::base::mark:
	    {
		session::buffer_state &st = s.buffers[s.cur];
		std::map<char, anchor_id>::const_iterator m =
		    st.marks.find(a.mark_name);
		const anchor *an = m == st.marks.end()
				   ? (const anchor *)0
				   : s.w.anchor_of(s.cur, m->second);
		if ( !an )
		    return fail(std::string("mark not set: '") + a.mark_name);
		base = text()->line_of(an->off);
		if ( base > n )
		    base = n;
		break;
	    }
	}
	long v = (long)base + a.offset;
	if ( v < 0 || (size_t)v > n || (v == 0 && !zero_ok && n > 0) )
	    return fail("invalid range");
	result = (size_t)v;
	return true;
    }

    // The lines a command acts on, [first, last]; an empty buffer's range
    // is 0,0.
    bool resolve_range(const line &l, default_range d, size_t &first,
		       size_t &last, bool zero_ok = false)
    {
	const size_t n = lines();
	if ( l.where.count == 0 )
	{
	    switch ( d )
	    {
		case default_range::current:
		    first = last = dot();
		    break;
		case default_range::whole:
		    first = n ? 1 : 0;
		    last = n;
		    break;
		case default_range::last:
		    first = last = n;
		    break;
		case default_range::current_next:
		    first = dot();
		    last = first + 1;
		    break;
	    }
	}
	else
	{
	    if ( !resolve(l.where.first, first, zero_ok) )
		return false;
	    if ( l.where.from_current && l.where.count == 2 )
		set_dot(first);
	    if ( !resolve(l.where.second, last, zero_ok) )
		return false;
	    if ( l.where.count == 1 )
		first = last;
	}
	if ( l.has_count )
	{
	    first = last;
	    last = first + l.count - 1;
	    if ( last > n )
		last = n;
	}
	if ( first > last )
	    return fail("backwards range");
	return true;
    }

    // ---- showing lines (p, nu, l)
    std::string numbered(size_t n) const
    {
	size_t width = 3;
	for ( size_t total = lines(); total >= 1000; total /= 10 )
	    ++width;
	std::string num = std::to_string(n);
	return std::string(width > num.size() ? width - num.size() : 0, ' ')
	       + num + " ";
    }
    static std::string listed(const std::string &t)
    {
	std::string r;
	for ( char c : t )
	{
	    unsigned char u = (unsigned char)c;
	    if ( u < 0x20 )
	    {
		r += '^';
		r += (char)(u + 64);
	    }
	    else if ( u == 0x7f )
		r += "^?";
	    else
		r += c;
	}
	return r + "$";
    }
    void show(size_t n, command how)
    {
	std::string t = line_text(n);
	if ( how == command::list )
	    t = listed(t);
	if ( how == command::number || s.opts.number )
	    t = numbered(n) + t;
	out.shown += t + "\n";
    }

    // ---- editing (every edit inside one checkpointed group)
    // Before a command line's first edit: one undo group per command line,
    // :g's included (every command it runs edits inside the one group).
    bool begin_change()
    {
	const madc::hub::document *d = s.w.doc_of(s.cur);
	if ( d && d->read_only() )
	    return fail("the buffer is read-only");
	if ( checkpointed )
	    return true;
	checkpointed = true;
	mutation_context mc(s.w);
	mc.doc_checkpoint(s.cur, madc::value());
	return true;
    }
    std::vector<std::string> take_lines(size_t first, size_t last) const
    {
	std::vector<std::string> r;
	for ( size_t k = first; k <= last && k != 0; ++k )
	    r.push_back(line_text(k));
	return r;
    }
    // Remove lines [first, last] with their line breaks; an unterminated
    // last line takes the break before it, so the text stays unterminated.
    void erase_lines(size_t first, size_t last)
    {
	const text_buffer *b = text();
	const size_t n = lines();
	size_t off = line_start(first);
	size_t end = last < n ? line_start(last + 1) : b->size();
	if ( last == n && first > 1 && unterminated() )
	    --off;
	mutation_context mc(s.w);
	mc.text_erase(s.cur, off, end - off);
	marks_deleted(first, last);
    }
    // Put `body` after line `after` (0: before the first). After an
    // unterminated last line the new lines stay unterminated too.
    void insert_after(size_t after, const std::vector<std::string> &body)
    {
	if ( body.empty() )
	    return;
	std::string joined;
	for ( size_t i = 0; i < body.size(); ++i )
	    joined += (i ? "\n" : "") + body[i];
	const size_t n = lines();
	size_t off = 0;
	std::string ins = joined + "\n";
	if ( after >= n && n > 0 )
	{
	    off = text()->size();
	    if ( unterminated() )
		ins = "\n" + joined;
	}
	else if ( after > 0 )
	    off = line_start(after + 1);
	mutation_context mc(s.w);
	mc.text_insert(s.cur, off, ins);
	marks_inserted(after, body.size());
    }
    void replace_line(size_t n, const std::string &with)
    {
	const text_buffer *b = text();
	size_t off = 0, len = 0;
	if ( !b->line_span(n, off, len) )
	    return;
	mutation_context mc(s.w);
	mc.text_replace(s.cur, off, len, with);
	marks_inserted(n, (size_t)std::count(with.begin(), with.end(), '\n'));
    }
    // Lines [first, last] become `body`; the last line's break stays as it
    // was.
    void replace_lines(size_t first, size_t last,
		       const std::vector<std::string> &body)
    {
	size_t loff = 0, llen = 0;
	if ( !text()->line_span(last, loff, llen) )
	    return;
	std::string joined;
	for ( size_t i = 0; i < body.size(); ++i )
	    joined += (i ? "\n" : "") + body[i];
	const size_t off = line_start(first);
	mutation_context mc(s.w);
	mc.text_replace(s.cur, off, loff + llen - off, joined);
	marks_deleted(first, last);
	marks_inserted(first - 1, body.size());
    }
    // A pattern as madc::regex compiles it, or the refusal.
    bool compile(const std::string &pattern, bool ignore_case,
		 std::unique_ptr<madc::regex> &re)
    {
	re.reset(new madc::regex(ignore_case ? "(?i)" + pattern : pattern));
	if ( !re->ok() )
	    return fail("invalid pattern '" + pattern + "': " + re->error());
	return true;
    }

    // The screen width of a line's leading blanks (tabs to 'tabstop').
    size_t indent_width(const std::string &t, size_t &lead) const
    {
	size_t width = 0;
	const size_t ts = s.opts.tabstop ? s.opts.tabstop : 8;
	for ( lead = 0; lead < t.size() && blank_byte(t[lead]); ++lead )
	    width = t[lead] == '\t' ? (width / ts + 1) * ts : width + 1;
	return width;
    }
    std::string make_indent(size_t width) const
    {
	const size_t ts = s.opts.tabstop ? s.opts.tabstop : 8;
	if ( s.opts.expandtab )
	    return std::string(width, ' ');
	return std::string(width / ts, '\t') + std::string(width % ts, ' ');
    }

    // ---- the commands
    bool goto_line(const line &l)
    {
	const size_t n = lines();
	size_t target = 0;
	if ( l.where.count == 0 )
	{
	    // An empty line: the next line (ex).
	    target = dot() + 1;
	    if ( target > n )
		return fail("at the end of the buffer");
	}
	else
	{
	    size_t first = 0;
	    if ( !resolve_range(l, default_range::current, first, target, true) )
		return false;
	    if ( target == 0 )
		target = n ? 1 : 0;	// :0 is the first line
	}
	if ( target == 0 )
	    return fail("no lines in the buffer");
	set_dot(target);
	show(target, command::print);
	return true;
    }

    bool print_lines(const line &l)
    {
	size_t first = 0, last = 0;
	if ( !resolve_range(l, default_range::current, first, last) )
	    return false;
	if ( last == 0 )
	    return fail("no lines in the buffer");
	for ( size_t k = first; k <= last; ++k )
	    show(k, l.code);
	set_dot(last);
	return true;
    }

    bool line_number(const line &l)
    {
	size_t first = 0, last = 0;
	if ( !resolve_range(l, default_range::last, first, last, true) )
	    return false;
	out.shown += std::to_string(last) + "\n";
	return true;
    }

    bool del(const line &l)
    {
	size_t first = 0, last = 0;
	if ( !resolve_range(l, default_range::current, first, last) )
	    return false;
	if ( last == 0 )
	    return fail("no lines in the buffer");
	if ( !begin_change() )
	    return false;
	erase_lines(first, last);
	const size_t n = lines();
	set_dot(first <= n ? first : n);
	return true;
    }

    bool join(const line &l)
    {
	size_t first = 0, last = 0;
	if ( !resolve_range(l, default_range::current_next, first, last) )
	    return false;
	const size_t n = lines();
	if ( l.where.count == 1 && !l.has_count )
	    last = first + 1;		// :Nj joins N with the next line
	if ( last > n )
	    last = n;
	if ( first == 0 || first >= last )
	    return true;		// nothing to join (ed)
	std::string joined = line_text(first);
	for ( size_t k = first + 1; k <= last; ++k )
	{
	    std::string next = line_text(k);
	    if ( !l.force )
	    {
		// vim: the joined line's leading blanks go; one space joins
		// them, none after a blank, before `)`, or for an empty line.
		size_t lead = 0;
		while ( lead < next.size() && blank_byte(next[lead]) )
		    ++lead;
		next.erase(0, lead);
		if ( !next.empty() && next[0] != ')' && !joined.empty()
		     && !blank_byte(joined[joined.size() - 1]) )
		    joined += ' ';
	    }
	    joined += next;
	}
	if ( !begin_change() )
	    return false;
	size_t off = line_start(first), loff = 0, llen = 0;
	text()->line_span(last, loff, llen);
	mutation_context mc(s.w);
	mc.text_replace(s.cur, off, loff + llen - off, joined);
	marks_deleted(first + 1, last);
	set_dot(first);
	return true;
    }

    bool move_or_copy(const line &l)
    {
	size_t first = 0, last = 0, to = 0;
	if ( !resolve_range(l, default_range::current, first, last) )
	    return false;
	if ( last == 0 )
	    return fail("no lines in the buffer");
	if ( !resolve(l.destination, to, true) )
	    return false;
	const bool moving = l.code == command::move;
	if ( moving && to >= first && to < last )
	    return fail("cannot move a range of lines into itself");
	std::vector<std::string> body = take_lines(first, last);
	if ( !begin_change() )
	    return false;
	if ( moving )
	{
	    erase_lines(first, last);
	    if ( to > last )
		to -= body.size();
	}
	insert_after(to, body);
	set_dot(to + body.size());
	return true;
    }

    bool shift(const line &l)
    {
	size_t first = 0, last = 0;
	if ( !resolve_range(l, default_range::current, first, last) )
	    return false;
	if ( last == 0 )
	    return fail("no lines in the buffer");
	if ( !begin_change() )
	    return false;
	const size_t step = (size_t)l.levels
			    * (s.opts.shiftwidth ? s.opts.shiftwidth
						 : s.opts.tabstop);
	for ( size_t k = first; k <= last; ++k )
	{
	    std::string t = line_text(k);
	    if ( t.empty() )
		continue;		// vim leaves an empty line alone
	    size_t lead = 0;
	    size_t width = indent_width(t, lead);
	    size_t now = l.code == command::shift_right ? width + step
			 : width > step ? width - step : 0;
	    std::string ind = make_indent(now);
	    if ( ind != t.substr(0, lead) )
		replace_line(k, ind + t.substr(lead));
	}
	set_dot(last);
	return true;
    }

    bool undo_redo(const line &l)
    {
	mutation_context mc(s.w);
	madc::value app;
	if ( l.code == command::undo ? !mc.doc_undo(s.cur, app)
				     : !mc.doc_redo(s.cur, app) )
	    return fail(l.code == command::undo ? "already at oldest change"
						: "already at newest change");
	return true;
    }

    bool mark(const line &l)
    {
	size_t first = 0, last = 0;
	if ( !resolve_range(l, default_range::current, first, last) )
	    return false;
	if ( last == 0 )
	    return fail("no lines in the buffer");
	session::buffer_state &st = s.buffers[s.cur];
	mutation_context mc(s.w);
	const char name = l.argument[0];
	std::map<char, anchor_id>::iterator m = st.marks.find(name);
	if ( m != st.marks.end() )
	    mc.anchor_remove(s.cur, m->second);
	st.marks[name] = mc.anchor_add(s.cur, line_start(last), false);
	return true;
    }

    bool marks()
    {
	session::buffer_state &st = s.buffers[s.cur];
	out.shown += "mark line  text\n";
	for ( const auto &m : st.marks )
	{
	    const anchor *an = s.w.anchor_of(s.cur, m.second);
	    if ( !an )
		continue;
	    size_t n = text()->line_of(an->off);
	    if ( n > lines() )
		n = lines();
	    std::string num = std::to_string(n);
	    out.shown += std::string(" ") + m.first
			 + std::string(num.size() < 6 ? 6 - num.size() : 0, ' ')
			 + num + "  " + line_text(n) + "\n";
	}
	return true;
    }

    // ---- search and substitute (slice 3)

    // One match's replacement text (design decision 6): `&` and `\0` the
    // match, `\1`-`\9` a group (unset: empty), `\n` and `\r` a line break
    // (sed's and vim's spellings), `\t` a tab, any other escaped character
    // itself (`\&`, `\\`).
    static std::string group_text(const std::string &t,
				  const std::vector<madc::regex::span> &g,
				  size_t k)
    {
	if ( k >= g.size() || g[k].begin == std::string::npos )
	    return std::string();
	return t.substr(g[k].begin, g[k].end - g[k].begin);
    }
    static std::string expand(const std::string &rep, const std::string &t,
			      const std::vector<madc::regex::span> &g)
    {
	std::string r;
	for ( size_t i = 0; i < rep.size(); ++i )
	{
	    const char c = rep[i];
	    if ( c == '&' )
		r += group_text(t, g, 0);
	    else if ( c == '\\' && i + 1 < rep.size() )
	    {
		const char e = rep[++i];
		if ( e >= '0' && e <= '9' )
		    r += group_text(t, g, (size_t)(e - '0'));
		else if ( e == 'n' || e == 'r' )
		    r += '\n';
		else if ( e == 't' )
		    r += '\t';
		else
		    r += e;
	    }
	    else
		r += c;
	}
	return r;
    }
    static std::string counted(size_t n, const char *one, const char *many)
    {
	return std::to_string(n) + " " + (n == 1 ? one : many);
    }

    // :s, :& and :&& (vim's :substitute): on each line of the range, the
    // first match — every one with `g` — replaced; `n` counts instead, `e`
    // makes no match no error, `i` / `I` set the case, a leading `&` keeps
    // the last :s's flags. An empty pattern is the last one used. The
    // current line ends on the last line changed.
    bool substitute(const line &l)
    {
	// The range first: a /re/ in it is the last pattern an empty one uses.
	size_t first = 0, last = 0;
	if ( !resolve_range(l, default_range::current, first, last) )
	    return false;
	std::string pattern = l.pattern;
	std::string replacement = l.replacement;
	std::string flags = l.flags;
	if ( l.code != command::substitute )
	{
	    if ( !s.last_sub.set )
		return fail("no previous substitute");
	    pattern = s.last_sub.pattern;
	    replacement = s.last_sub.replacement;
	    if ( l.code == command::repeat_sub_flags )
		flags = "&" + flags;
	}
	if ( !flags.empty() && flags[0] == '&' )
	    flags = s.last_sub.flags + flags.substr(1);
	bool every = false, count_only = false, quiet = false, icase = false;
	for ( char f : flags )
	{
	    if ( f == 'g' )
		every = true;
	    else if ( f == 'n' )
		count_only = true;
	    else if ( f == 'e' )
		quiet = true;
	    else if ( f == 'i' )
		icase = true;
	    else if ( f == 'I' )
		icase = false;
	}
	if ( pattern.empty() )
	    pattern = s.last_pattern;
	if ( pattern.empty() )
	    return fail("no previous pattern");
	std::unique_ptr<madc::regex> re;
	if ( !compile(pattern, icase, re) )
	    return false;
	s.last_pattern = pattern;
	s.last_sub.set = true;
	s.last_sub.pattern = pattern;
	s.last_sub.replacement = replacement;
	s.last_sub.flags = flags;

	size_t matches = 0, hit_lines = 0, last_hit = 0;
	size_t added = 0;		// lines the replacements' breaks made
	for ( size_t k = first; k <= last && k != 0; ++k )
	{
	    const size_t at = k + added;
	    const std::string t = line_text(at);
	    std::vector<std::vector<madc::regex::span> > m = re->each_groups(t);
	    if ( m.empty() )
		continue;
	    if ( !every )
		m.resize(1);
	    matches += m.size();
	    ++hit_lines;
	    if ( count_only )
		continue;
	    std::string r;
	    size_t from = 0;
	    for ( const std::vector<madc::regex::span> &g : m )
	    {
		r += t.substr(from, g[0].begin - from);
		r += expand(replacement, t, g);
		from = g[0].end;
	    }
	    r += t.substr(from);
	    if ( !begin_change() )
		return false;
	    replace_line(at, r);
	    const size_t breaks = (size_t)std::count(r.begin(), r.end(), '\n');
	    added += breaks;
	    last_hit = at + breaks;
	}
	if ( under_global )
	{
	    // :g reports for every line together (no match on one line is
	    // not an error there).
	    global_matches += matches;
	    global_lines += hit_lines;
	    if ( last_hit )
		set_dot(last_hit);
	    return true;
	}
	if ( matches == 0 )
	    return quiet ? true : fail("pattern not found: " + pattern);
	if ( count_only )
	{
	    out.shown += counted(matches, "match", "matches") + " on "
			 + counted(hit_lines, "line", "lines") + "\n";
	    return true;
	}
	set_dot(last_hit);
	return true;
    }

    // :g and :v (vim's :global; `g!` is `v`): mark the range's lines whose
    // text matches (v: does not), then run the command on each marked line
    // still there, top first, the current line set to it — ONE undo group
    // for all. The marks follow the commands' edits (marks_deleted /
    // marks_inserted), so a line a command deleted or joined away is not
    // run.
    bool global(const line &l)
    {
	if ( under_global )
	    return fail("cannot nest :global");
	size_t first = 0, last = 0;
	if ( !resolve_range(l, default_range::whole, first, last) )
	    return false;
	const std::string pattern = l.pattern.empty() ? s.last_pattern
						      : l.pattern;
	if ( pattern.empty() )
	    return fail("no previous pattern");
	std::unique_ptr<madc::regex> re;
	if ( !compile(pattern, false, re) )
	    return false;
	s.last_pattern = pattern;
	line sub;
	std::string error;
	if ( !parse(l.subcommand.empty() ? std::string("p") : l.subcommand,
		    sub, error) )
	    return fail(error);
	switch ( sub.code )
	{
	    case command::global: case command::vglobal:
		return fail("cannot nest :global");
	    case command::append: case command::insert: case command::change:
	    case command::undo: case command::redo:
		return fail(std::string(command_name(sub.code))
			    + " cannot run under :global");
	    default:
		break;
	}
	const bool wanted = l.code == command::global && !l.force;
	global_marks.clear();
	global_bias = 0;
	for ( size_t k = first; k <= last && k != 0; ++k )
	    if ( re->search(line_text(k)) == wanted )
		global_marks.push_back((long long)k);
	if ( global_marks.empty() )
	{
	    out.shown += (wanted ? "pattern not found: "
				 : "pattern found in every line: ")
			 + pattern + "\n";
	    return true;
	}
	under_global = true;
	bool ok = true;
	while ( ok && !global_marks.empty() )
	{
	    const size_t at = (size_t)(global_marks.front() + global_bias);
	    global_marks.pop_front();
	    set_dot(at);
	    ok = execute(sub);
	}
	under_global = false;
	global_marks.clear();
	if ( !ok )
	    return false;
	if ( sub.code == command::substitute || sub.code == command::repeat_sub
	     || sub.code == command::repeat_sub_flags )
	{
	    const bool counting = s.last_sub.flags.find('n') != std::string::npos;
	    const bool quiet = s.last_sub.flags.find('e') != std::string::npos;
	    if ( global_matches == 0 && !quiet )
		return fail("pattern not found: " + s.last_sub.pattern);
	    if ( counting && global_matches )
		out.shown += counted(global_matches, "match", "matches") + " on "
			     + counted(global_lines, "line", "lines") + "\n";
	}
	return true;
    }

    // :sort (vim's ex_sort): the range's lines, stable — by text, or with
    // `n` `x` `o` `b` `f` by the line's first number of that kind (a line
    // without one first; `f`: the text as a float, an empty line first);
    // `i` ignores case; a /pattern/ keys each line on the text after its
    // match (`r`: the match), a line without one keyed empty; `!` reverses;
    // `u` keeps the first of equal lines (the whole line, `i` ignoring
    // case). The current line is the range's first.
    struct sort_row
    {
	size_t      at;		// the original order
	std::string text;
	std::string key;
	bool        numbered;
	long long   number;
	double      real;
    };
    static int compare_text(const std::string &a, const std::string &b,
			    bool icase)
    {
	const size_t n = a.size() < b.size() ? a.size() : b.size();
	for ( size_t i = 0; i < n; ++i )
	{
	    int x = (unsigned char)a[i], y = (unsigned char)b[i];
	    if ( icase )
	    {
		x = tolower(x);
		y = tolower(y);
	    }
	    if ( x != y )
		return x < y ? -1 : 1;
	}
	return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
    }
    // vim's number per kind: the first digit of the base (`x`: a 0x
    // prefix, `b`: a 0b prefix allowed), negative after a `-`.
    static void sort_number(sort_row &r, char kind)
    {
	const std::string &k = r.key;
	if ( kind == 'f' )
	{
	    size_t i = 0;
	    while ( i < k.size() && blank_byte(k[i]) )
		++i;
	    if ( i < k.size() && k[i] == '+' )
		for ( ++i; i < k.size() && blank_byte(k[i]); ++i )
		    ;
	    r.numbered = true;
	    r.real = i >= k.size() ? -DBL_MAX : strtod(k.c_str() + i, 0);
	    return;
	}
	const int base = kind == 'x' ? 16 : kind == 'o' ? 8 : kind == 'b' ? 2
							  : 10;
	size_t i = 0;
	for ( ; i < k.size(); ++i )
	{
	    const char c = k[i];
	    if ( base == 16 ? isxdigit((unsigned char)c) != 0
			    : c >= '0' && c < '0' + (base < 10 ? base : 10) )
		break;
	}
	r.numbered = i < k.size();
	if ( !r.numbered )
	    return;
	const bool negative = i > 0 && k[i - 1] == '-';
	if ( base == 2 && k[i] == '0' && i + 2 < k.size()
	     && (k[i + 1] == 'b' || k[i + 1] == 'B')
	     && (k[i + 2] == '0' || k[i + 2] == '1') )
	    i += 2;
	const long long v = strtoll(k.c_str() + i, 0, base);
	r.number = negative ? -v : v;
    }
    bool sort(const line &l)
    {
	size_t first = 0, last = 0;
	if ( !resolve_range(l, default_range::whole, first, last) )
	    return false;
	if ( last == 0 )
	    return true;		// an empty buffer: nothing to sort
	bool icase = false, unique = false, on_match = false;
	char kind = 0;
	for ( char f : l.flags )
	{
	    if ( f == 'i' )
		icase = true;
	    else if ( f == 'u' )
		unique = true;
	    else if ( f == 'r' )
		on_match = true;
	    else
		kind = f;
	}
	std::unique_ptr<madc::regex> re;
	if ( l.has_pattern )
	{
	    const std::string pattern = l.pattern.empty() ? s.last_pattern
							  : l.pattern;
	    if ( pattern.empty() )
		return fail("no previous pattern");
	    if ( !compile(pattern, false, re) )
		return false;
	    s.last_pattern = pattern;
	}
	std::vector<sort_row> rows;
	for ( size_t k = first; k <= last; ++k )
	{
	    sort_row r;
	    r.at = k;
	    r.text = line_text(k);
	    r.numbered = false;
	    r.number = 0;
	    r.real = 0;
	    r.key = r.text;
	    if ( re )
	    {
		std::vector<madc::regex::span> m = re->each(r.text);
		if ( m.empty() )
		    r.key.clear();
		else if ( on_match )
		    r.key = r.text.substr(m[0].begin, m[0].end - m[0].begin);
		else
		    r.key = r.text.substr(m[0].end);
	    }
	    if ( kind )
		sort_number(r, kind);
	    rows.push_back(r);
	}
	std::stable_sort(rows.begin(), rows.end(),
			 [kind, icase](const sort_row &a, const sort_row &b) {
	    if ( kind == 'f' )
		return a.real < b.real;
	    if ( kind )
	    {
		if ( a.numbered != b.numbered )
		    return !a.numbered;
		return a.number < b.number;
	    }
	    return compare_text(a.key, b.key, icase) < 0;
	});
	if ( l.force )
	    std::reverse(rows.begin(), rows.end());
	std::vector<std::string> sorted;
	for ( const sort_row &r : rows )
	    if ( !unique || sorted.empty()
		 || compare_text(r.text, sorted.back(), icase) != 0 )
		sorted.push_back(r.text);
	if ( sorted != take_lines(first, last) )
	{
	    if ( !begin_change() )
		return false;
	    replace_lines(first, last, sorted);
	}
	set_dot(first);
	return true;
    }

    // :retab (vim's ex_retab): each run of blanks that holds a tab — with
    // `!`, a run of two spaces or more too — rewritten for the new tabstop
    // (`:ret N`; none or 0: the current one), its end column kept: spaces
    // under 'expandtab', else tabs then spaces; then 'tabstop' is the new
    // one. Columns count tabs at the old tabstop; a UTF-8 continuation
    // byte takes no column.
    std::string retabbed(const std::string &t, size_t old_ts, size_t new_ts,
			 bool force) const
    {
	std::string r;
	size_t vcol = 0, start_vcol = 0, start_col = 0, spaces = 0;
	bool got_tab = false;
	for ( size_t col = 0; ; ++col )
	{
	    const bool white = col < t.size() && blank_byte(t[col]);
	    if ( white )
	    {
		if ( !got_tab && spaces == 0 )
		{
		    start_vcol = vcol;
		    start_col = col;
		}
		if ( t[col] == ' ' )
		    ++spaces;
		else
		    got_tab = true;
	    }
	    else
	    {
		if ( got_tab || spaces )
		{
		    const size_t len = vcol - start_vcol;
		    size_t nsp = len, ntabs = 0;
		    if ( !s.opts.expandtab )
		    {
			const size_t to_stop = new_ts - start_vcol % new_ts;
			if ( nsp >= to_stop )
			{
			    nsp -= to_stop;
			    ++ntabs;
			}
			ntabs += nsp / new_ts;
			nsp %= new_ts;
		    }
		    const bool rewrite = (got_tab || (force && spaces > 1))
					 && (s.opts.expandtab || got_tab
					     || nsp + ntabs < len);
		    if ( rewrite )
			r += std::string(ntabs, '\t') + std::string(nsp, ' ');
		    else
			r += t.substr(start_col, col - start_col);
		}
		got_tab = false;
		spaces = 0;
		if ( col >= t.size() )
		    break;
		r += t[col];
	    }
	    if ( t[col] == '\t' )
		vcol += old_ts - vcol % old_ts;
	    else if ( ((unsigned char)t[col] & 0xC0) != 0x80 )
		++vcol;
	}
	return r;
    }
    bool retab(const line &l)
    {
	size_t first = 0, last = 0;
	if ( !resolve_range(l, default_range::whole, first, last) )
	    return false;
	const size_t old_ts = s.opts.tabstop ? s.opts.tabstop : 8;
	const size_t new_ts = l.count ? l.count : old_ts;
	for ( size_t k = first; k <= last && k != 0; ++k )
	{
	    const std::string t = line_text(k);
	    const std::string r = retabbed(t, old_ts, new_ts, l.force);
	    if ( r == t )
		continue;
	    if ( !begin_change() )
		return false;
	    replace_line(k, r);
	}
	s.opts.tabstop = new_ts;
	return true;
    }

    // ---- files and buffers (slice 4)

    // A typed file name as a path: `~` against the home, then against the
    // session's directory.
    std::string path_of(const std::string &typed) const
    {
	std::string p;
	if ( !madc::detail::host_path_expand_home(typed, p) )
	    p = typed;
	return madc::detail::host_path_join(s.dir, p);
    }
    // A path as a message names it: below the session's directory, the part
    // below it (vim's); untitled, [No Name].
    std::string shown_name(const std::string &path) const
    {
	if ( path.empty() )
	    return "[No Name]";
	const std::string d = madc::detail::canonical_path_for_compare(s.dir);
	const std::string p = madc::detail::canonical_path_for_compare(path);
	size_t at = 0;
	if ( madc::detail::host_path_within(d, p, &at) )
	    return p.substr(at);
	return path;
    }
    static bool same_path(const std::string &a, const std::string &b)
    {
	return !a.empty() && !b.empty()
	       && madc::detail::canonical_path_for_compare(a)
		  == madc::detail::canonical_path_for_compare(b);
    }
    static size_t text_lines(const std::string &t)
    {
	size_t n = (size_t)std::count(t.begin(), t.end(), '\n');
	return !t.empty() && t[t.size() - 1] != '\n' ? n + 1 : n;
    }
    // vim's file message: "name" [New] NL, NB[ written / appended].
    void file_message(const std::string &path, const std::string &text,
		      bool fresh, const char *done)
    {
	out.shown += "\"" + shown_name(path) + "\" " + (fresh ? "[New] " : "")
		     + std::to_string(text_lines(text)) + "L, "
		     + std::to_string(text.size()) + "B"
		     + (done ? std::string(" ") + done : std::string()) + "\n";
    }
    // A file's text as lines (a final line break ends the last line).
    static std::vector<std::string> split_lines(const std::string &t)
    {
	std::vector<std::string> r;
	size_t at = 0;
	while ( at < t.size() )
	{
	    size_t nl = t.find('\n', at);
	    if ( nl == std::string::npos )
		nl = t.size();
	    r.push_back(t.substr(at, nl - at));
	    at = nl + 1;
	}
	return r;
    }
    std::string range_text(size_t first, size_t last) const
    {
	std::string t;
	for ( size_t k = first; k <= last && k != 0; ++k )
	    t += line_text(k) + "\n";
	return t;
    }

    // A shell command (design decision 10): `/bin/sh -c` (`cmd /c` on
    // Windows) through madc::Process, in the session's directory, `input`
    // its stdin; its stdout then stderr into `output`.
    bool run_shell(const std::string &command, const std::string &input,
		   std::string &output, int &status)
    {
	madc::ProcessOptions o;
#ifdef _WIN32
	const char *shell = "cmd";
	o.args.push_back("/c");
#else
	const char *shell = "/bin/sh";
	o.args.push_back("-c");
#endif
	o.args.push_back(command);
	o.working_directory = s.dir;
	madc::Process p(madc::DataSource(std::string("exec://") + shell), o);
	if ( !p.start() )
	    return fail(std::string("the shell '") + shell + "' did not start");
	madc::MemoryDataChannel in(std::vector<unsigned char>(input.begin(),
							      input.end()));
	madc::MemoryDataChannel stdout_text, stderr_text;
	// The pump's false covers a nonzero exit and a command that stopped
	// reading its stdin (`:w !head -1`); both ran — only an unreaped
	// child (no status) did not.
	madc::ProcessPumpResult r;
	madc::pump_process(in, p, stdout_text, &stderr_text, r);
	if ( r.exit_status < 0 )
	    return fail("the shell command did not run: " + command);
	output.assign(stdout_text.bytes().begin(), stdout_text.bytes().end());
	output.append(stderr_text.bytes().begin(), stderr_text.bytes().end());
	status = r.exit_status;
	return true;
    }
    // `%` in a shell command is the current buffer's name (vim's one
    // useful expansion); `\%` is a `%`.
    bool expand_command(const std::string &typed, std::string &command)
    {
	command.clear();
	for ( size_t i = 0; i < typed.size(); ++i )
	{
	    if ( typed[i] == '\\' && i + 1 < typed.size() && typed[i + 1] == '%' )
	    {
		command += '%';
		++i;
	    }
	    else if ( typed[i] == '%' )
	    {
		const madc::hub::document *d = s.cur ? s.w.doc_of(s.cur) : 0;
		if ( !d || d->path().empty() )
		    return fail("no file name for '%'");
		command += shown_name(d->path());
	    }
	    else
		command += typed[i];
	}
	return true;
    }
    void shell_output(const std::string &output, int status)
    {
	out.shown += output;
	if ( !output.empty() && output[output.size() - 1] != '\n' )
	    out.shown += "\n";
	if ( status != 0 )
	    out.shown += "shell returned " + std::to_string(status) + "\n";
    }

    // :!cmd
    bool shell(const line &l)
    {
	std::string command, output;
	int status = 0;
	if ( !expand_command(l.argument, command)
	     || !run_shell(command, std::string(), output, status) )
	    return false;
	shell_output(output, status);
	return true;
    }

    // :e[!] [FILE] — FILE's buffer current: the open one, else read, else
    // a new one named FILE; `#` the alternate. No FILE: the current file
    // read again (refused over changes unless !).
    bool edit(const line &l)
    {
	const std::string &arg = l.argument;
	if ( arg == "#" )
	{
	    if ( !s.alternate || !s.w.has_doc(s.alternate) )
		return fail("no alternate file");
	    s.use(s.alternate);
	    file_message(s.w.doc_of(s.cur)->path(), text_of_buffer(), false, 0);
	    return true;
	}
	if ( arg.empty() )
	{
	    const madc::hub::document *d = s.cur ? s.w.doc_of(s.cur) : 0;
	    if ( !d )
		return fail("no buffer (:e FILE opens one)");
	    if ( d->path().empty() )
		return fail("no file name");
	    if ( s.w.doc_modified(s.cur) && !l.force )
		return fail("no write since last change (add ! to override)");
	    const std::string path = d->path();
	    if ( !madc::document_reload(s.w, s.cur, path) )
		return fail("cannot read \"" + shown_name(path) + "\"");
	    file_message(path, text()->text(), false, 0);
	    return true;
	}
	const std::string path = path_of(arg);
	madc::hub::entity_id e = madc::document_find(s.w, path);
	if ( e )
	{
	    s.use(e);
	    file_message(path, text_of_buffer(), false, 0);
	    return true;
	}
	e = madc::document_open(s.w, path);
	if ( e )
	{
	    s.use(e);
	    file_message(path, s.w.text_of(e)->text(), false, 0);
	    return true;
	}
	if ( madc::detail::host_path_type_of(path)
	     != madc::detail::host_path_type::none )
	    return fail("cannot read \"" + shown_name(path) + "\"");
	s.use(madc::document_new(s.w, path));
	out.shown += "\"" + shown_name(path) + "\" [New]\n";
	return true;
    }

    // :r [FILE] / :r !cmd — the text after the addressed line (0: before
    // the first); the current line its last.
    bool read(const line &l)
    {
	size_t first = 0, last = 0;
	if ( !resolve_range(l, default_range::current, first, last, true) )
	    return false;
	std::string text;
	std::string path;
	if ( l.shell_form )
	{
	    std::string command;
	    int status = 0;
	    if ( !expand_command(l.argument, command)
		 || !run_shell(command, std::string(), text, status) )
		return false;
	    if ( status != 0 )
		out.shown += "shell returned " + std::to_string(status) + "\n";
	}
	else
	{
	    const madc::hub::document *d = s.w.doc_of(s.cur);
	    path = l.argument.empty() ? d->path() : path_of(l.argument);
	    if ( path.empty() )
		return fail("no file name");
	    if ( !madc::document_read_file(path, text) )
		return fail("cannot read \"" + shown_name(path) + "\"");
	}
	const std::vector<std::string> body = split_lines(text);
	if ( !body.empty() )
	{
	    if ( !begin_change() )
		return false;
	    insert_after(last, body);
	    set_dot(last + body.size());
	}
	if ( !path.empty() )
	    file_message(path, text, false, 0);
	return true;
    }

    // Any buffer with changes not written blocks quitting (vim's E37 for
    // the current, E162 for another) — every document of the world.
    bool quit_allowed(bool force, madc::hub::entity_id written)
    {
	if ( force )
	    return true;
	if ( s.cur && s.cur != written && s.w.doc_modified(s.cur) )
	    return fail("no write since last change (add ! to override)");
	const std::vector<madc::hub::entity_id> all = s.w.docs();
	for ( size_t i = 0; i < all.size(); ++i )
	    if ( all[i] != written && s.w.doc_modified(all[i]) )
		return fail("no write since last change for buffer \""
			    + shown_name(s.w.doc_of(all[i])->path())
			    + "\" (add ! to override)");
	return true;
    }
    bool quit(const line &l)
    {
	if ( !quit_allowed(l.force, 0) )
	    return false;
	out.quit = true;
	return true;
    }

    // :w :wq :x :up — the range (default the whole buffer) to FILE or the
    // buffer's own file; `>>` appends; :w !cmd pipes it to a command. :x
    // and :up write only a buffer with changes; :wq and :x then quit.
    bool write(const line &l)
    {
	size_t first = 0, last = 0;
	if ( !resolve_range(l, default_range::whole, first, last, true) )
	    return false;
	if ( l.shell_form )
	{
	    std::string command, output;
	    int status = 0;
	    if ( !expand_command(l.argument, command)
		 || !run_shell(command, range_text(first, last), output, status) )
		return false;
	    shell_output(output, status);
	    return true;
	}
	const madc::hub::document *d = s.w.doc_of(s.cur);
	const bool append = l.flags == ">>";
	std::string target = l.argument.empty() ? d->path()
						: path_of(l.argument);
	if ( target.empty() )
	    return fail("no file name");
	const bool whole = first <= 1 && last == lines();
	if ( d->path().empty() && !append && whole )
	    madc::document_set_path(s.w, s.cur, target);	// vim names it
	const bool own = same_path(target, d->path());
	const bool when_changed = l.code == command::exit
				  || l.code == command::update;
	if ( !(when_changed && own && !s.w.doc_modified(s.cur)) )
	{
	    if ( own && !whole && !append && !l.force )
		return fail("use ! to write a partial buffer");
	    if ( own && d->read_only() && !l.force )
		return fail("the buffer is read-only (add ! to override)");
	    const bool fresh = madc::detail::host_path_type_of(target)
			       == madc::detail::host_path_type::none;
	    if ( !own && !append && !fresh && !l.force )
		return fail("file exists (add ! to override)");
	    const std::string text = own && whole && !append ? text_of_buffer()
							     : range_text(first, last);
	    const bool written = own && whole && !append
				 ? madc::document_save(s.w, s.cur)
				 : madc::document_write_file(target, text, append);
	    if ( !written )
		return fail("cannot write \"" + shown_name(target) + "\"");
	    file_message(target, text, fresh, append ? "appended" : "written");
	}
	if ( l.code == command::write_quit || l.code == command::exit )
	{
	    if ( !quit_allowed(l.force, s.cur) )
		return false;
	    out.quit = true;
	}
	return true;
    }
    std::string text_of_buffer() const
    {
	const text_buffer *b = text();
	return b ? b->text() : std::string();
    }

    // :sav[!] FILE — the buffer renamed FILE and written there.
    bool saveas(const line &l)
    {
	if ( l.argument.empty() )
	    return fail("saveas needs a file name");
	const std::string target = path_of(l.argument);
	const bool fresh = madc::detail::host_path_type_of(target)
			   == madc::detail::host_path_type::none;
	if ( !fresh && !l.force && !same_path(target, s.w.doc_of(s.cur)->path()) )
	    return fail("file exists (add ! to override)");
	if ( !madc::document_set_path(s.w, s.cur, target)
	     || !madc::document_save(s.w, s.cur) )
	    return fail("cannot write \"" + shown_name(target) + "\"");
	file_message(target, text_of_buffer(), fresh, "written");
	return true;
    }

    // ---- the buffer list
    // The world's documents in :ls order, numbering any first seen.
    std::vector<madc::hub::entity_id> numbered_docs()
    {
	std::vector<madc::hub::entity_id> all = s.w.docs();
	std::vector<std::pair<size_t, madc::hub::entity_id> > by;
	for ( size_t i = 0; i < all.size(); ++i )
	    by.push_back(std::make_pair(s.number_of(all[i]), all[i]));
	std::sort(by.begin(), by.end());
	std::vector<madc::hub::entity_id> r;
	for ( size_t i = 0; i < by.size(); ++i )
	    r.push_back(by[i].second);
	return r;
    }
    // A buffer's current line as :ls shows it (vim's: 1 for an empty one).
    size_t listed_line(madc::hub::entity_id e)
    {
	const text_buffer *b = s.w.text_of(e);
	const size_t n = b ? b->line_count() : 0;
	size_t at = n;
	std::map<madc::hub::entity_id, session::buffer_state>::const_iterator st =
	    s.buffers.find(e);
	const anchor *a = st == s.buffers.end() || !st->second.dot
			  ? (const anchor *)0 : s.w.anchor_of(e, st->second.dot);
	if ( a && b )
	    at = b->line_of(a->off);
	if ( at > n )
	    at = n;
	return at ? at : 1;
    }
    // :ls (vim's buflist_list): number, `%` current / `#` alternate, `a`
    // shown / `h` hidden, `=` read-only, `+` changed, the name, its line.
    bool list_buffers()
    {
	const std::vector<madc::hub::entity_id> all = numbered_docs();
	for ( size_t i = 0; i < all.size(); ++i )
	{
	    const madc::hub::entity_id e = all[i];
	    const madc::hub::document *d = s.w.doc_of(e);
	    std::string num = std::to_string(s.number_of(e));
	    std::string row = std::string(num.size() < 3 ? 3 - num.size() : 0, ' ')
			      + num + " ";
	    row += e == s.cur ? '%' : e == s.alternate ? '#' : ' ';
	    row += e == s.cur ? 'a' : ' ';
	    row += d->read_only() ? '=' : ' ';
	    row += s.w.doc_modified(e) ? '+' : ' ';
	    row += " \"" + shown_name(d->path()) + "\"";
	    row += std::string(row.size() < 40 ? 40 - row.size() : 1, ' ');
	    out.shown += row + "line " + std::to_string(listed_line(e)) + "\n";
	}
	return true;
    }
    // :b N / :b # / :b NAME (a unique part of a buffer's name).
    bool buffer(const line &l)
    {
	const std::string &arg = l.argument;
	if ( arg.empty() )
	    return s.cur ? true : fail("no buffer (:e FILE opens one)");
	const std::vector<madc::hub::entity_id> all = numbered_docs();
	if ( arg == "#" )
	{
	    if ( !s.alternate || !s.w.has_doc(s.alternate) )
		return fail("no alternate file");
	    s.use(s.alternate);
	    return true;
	}
	if ( arg.find_first_not_of("0123456789") == std::string::npos )
	{
	    const size_t want = (size_t)strtoul(arg.c_str(), 0, 10);
	    for ( size_t i = 0; i < all.size(); ++i )
		if ( s.number_of(all[i]) == want )
		{
		    s.use(all[i]);
		    return true;
		}
	    return fail("buffer " + arg + " does not exist");
	}
	madc::hub::entity_id found = 0;
	for ( size_t i = 0; i < all.size(); ++i )
	    if ( shown_name(s.w.doc_of(all[i])->path()).find(arg)
		 != std::string::npos )
	    {
		if ( found )
		    return fail("more than one match for " + arg);
		found = all[i];
	    }
	if ( !found )
	    return fail("no matching buffer for " + arg);
	s.use(found);
	return true;
    }
    // :bn / :bp — the next or previous buffer by number, wrapping.
    bool cycle(const line &l)
    {
	const std::vector<madc::hub::entity_id> all = numbered_docs();
	if ( all.empty() )
	    return fail("no buffer (:e FILE opens one)");
	size_t at = 0;
	for ( size_t i = 0; i < all.size(); ++i )
	    if ( all[i] == s.cur )
		at = i;
	const size_t n = all.size();
	const size_t steps = l.has_count ? l.count % n : 1;
	at = l.code == command::bnext ? (at + steps) % n : (at + n - steps) % n;
	s.use(all[at]);
	return true;
    }

    // :cd [DIR] (none: the home; `-`: the one before) and :pwd — the
    // session's directory only.
    bool cd(const line &l)
    {
	std::string to;
	if ( l.argument == "-" )
	{
	    if ( s.previous_dir.empty() )
		return fail("no previous directory");
	    to = s.previous_dir;
	}
	else
	    to = path_of(l.argument.empty() ? std::string("~") : l.argument);
	if ( madc::detail::host_path_type_of(to)
	     != madc::detail::host_path_type::directory )
	    return fail("cannot change directory: " + l.argument);
	const std::string real = madc::detail::resolve_real_path(to.c_str());
	s.previous_dir = s.dir;
	s.dir = real.empty() ? to : real;
	return true;
    }

    // :set (design decision 3's four options), vim's forms: `opt` (a
    // boolean on; a number shown), `noopt`, `invopt` / `opt!`, `opt?`,
    // `opt=N` / `opt:N`, `opt&` (the default); no argument: the options
    // that differ from their defaults; `all`: every one.
    enum class option_code : unsigned char { number, tabstop, shiftwidth,
					     expandtab };
    struct option_row
    {
	const char *name;
	const char *short_name;
	option_code code;
	bool boolean;
    };
    static const option_row *option_named(const std::string &word)
    {
	static const option_row rows[] = {
	    { "number", "nu", option_code::number, true },
	    { "tabstop", "ts", option_code::tabstop, false },
	    { "shiftwidth", "sw", option_code::shiftwidth, false },
	    { "expandtab", "et", option_code::expandtab, true },
	};
	for ( const option_row &r : rows )
	    if ( word == r.name || word == r.short_name )
		return &r;
	return 0;
    }
    bool *flag_of(option_code c)
    {
	return c == option_code::number ? &s.opts.number
	     : c == option_code::expandtab ? &s.opts.expandtab : 0;
    }
    size_t *size_of(option_code c)
    {
	return c == option_code::tabstop ? &s.opts.tabstop
	     : c == option_code::shiftwidth ? &s.opts.shiftwidth : 0;
    }
    std::string option_shown(const option_row &r)
    {
	if ( r.boolean )
	    return (*flag_of(r.code) ? "  " : "no") + std::string(r.name);
	return "  " + std::string(r.name) + "=" + std::to_string(*size_of(r.code));
    }
    bool set(const line &l)
    {
	static const char *const all_names[] = { "expandtab", "number",
						 "shiftwidth", "tabstop" };
	const options defaults;
	std::vector<std::string> words;
	{
	    reader r(l.argument);
	    while ( r.skip_blanks(), !r.end() )
	    {
		std::string w;
		while ( !r.end() && !blank(r.peek()) )
		    w += r.s[r.at++];
		words.push_back(w);
	    }
	}
	if ( words.empty() || (words.size() == 1 && words[0] == "all") )
	{
	    out.shown += "--- Options ---\n";
	    for ( const char *name : all_names )
	    {
		const option_row &r = *option_named(name);
		const bool differs = r.boolean
		    ? *flag_of(r.code) != (r.code == option_code::number
					   ? defaults.number : defaults.expandtab)
		    : *size_of(r.code) != (r.code == option_code::tabstop
					   ? defaults.tabstop : defaults.shiftwidth);
		if ( differs || !words.empty() )
		    out.shown += option_shown(r) + "\n";
	    }
	    return true;
	}
	for ( const std::string &w : words )
	{
	    size_t cut = w.find_first_of("=:?!&");
	    std::string name = w.substr(0, cut);
	    const std::string rest = cut == std::string::npos ? std::string()
							       : w.substr(cut);
	    bool to_no = false, invert = false;
	    const option_row *r = option_named(name);
	    if ( !r && name.compare(0, 2, "no") == 0 )
	    {
		r = option_named(name.substr(2));
		to_no = r != 0;
	    }
	    if ( !r && name.compare(0, 3, "inv") == 0 )
	    {
		r = option_named(name.substr(3));
		invert = r != 0;
	    }
	    if ( !r )
		return fail("unknown option: " + name);
	    if ( rest == "?" || (!r->boolean && rest.empty()) )
	    {
		out.shown += option_shown(*r) + "\n";
		continue;
	    }
	    if ( rest == "&" )
	    {
		if ( r->boolean )
		    *flag_of(r->code) = r->code == option_code::number
					? defaults.number : defaults.expandtab;
		else
		    *size_of(r->code) = r->code == option_code::tabstop
					? defaults.tabstop : defaults.shiftwidth;
		continue;
	    }
	    if ( r->boolean )
	    {
		if ( rest == "!" )
		    invert = true;
		else if ( !rest.empty() )
		    return fail("invalid argument: " + w);
		bool &f = *flag_of(r->code);
		f = invert ? !f : !to_no;
		continue;
	    }
	    if ( to_no || invert || rest.size() < 2
		 || (rest[0] != '=' && rest[0] != ':')
		 || rest.find_first_not_of("0123456789", 1) != std::string::npos )
		return fail("invalid argument: " + w);
	    const size_t v = (size_t)strtoul(rest.c_str() + 1, 0, 10);
	    if ( v == 0 && r->code == option_code::tabstop )
		return fail("argument must be positive: " + w);
	    *size_of(r->code) = v;
	}
	return true;
    }

    bool want_text(const line &l)
    {
	size_t first = 0, last = 0;
	const bool zero_ok = l.code != command::change;
	if ( !resolve_range(l, default_range::current, first, last, zero_ok) )
	    return false;
	if ( l.code == command::change && last == 0 )
	    return fail("no lines in the buffer");
	const madc::hub::document *d = s.w.doc_of(s.cur);
	if ( d && d->read_only() )
	    return fail("the buffer is read-only");
	s.pending.code = l.code;
	s.pending.first = first;
	s.pending.last = last;
	out.wants_text = true;
	return true;
    }

    bool execute(const line &l)
    {
	// The commands that need no buffer: files, buffers, the directory,
	// the options, the shell.
	switch ( l.code )
	{
	    case command::edit:
		return edit(l);
	    case command::quit:
		return quit(l);
	    case command::buffers:
		return list_buffers();
	    case command::buffer:
		return buffer(l);
	    case command::bnext: case command::bprevious:
		return cycle(l);
	    case command::cd:
		return cd(l);
	    case command::pwd:
		out.shown += s.dir + "\n";
		return true;
	    case command::set:
		return set(l);
	    case command::shell:
		return shell(l);
	    default:
		break;
	}
	if ( !s.cur )
	    return fail("no buffer (:e FILE opens one)");
	switch ( l.code )
	{
	    case command::none:
		return goto_line(l);
	    case command::print: case command::number: case command::list:
		return print_lines(l);
	    case command::line_number:
		return line_number(l);
	    case command::del:
		return del(l);
	    case command::join:
		return join(l);
	    case command::move: case command::copy:
		return move_or_copy(l);
	    case command::shift_right: case command::shift_left:
		return shift(l);
	    case command::undo: case command::redo:
		return undo_redo(l);
	    case command::mark:
		return mark(l);
	    case command::marks:
		return marks();
	    case command::append: case command::insert: case command::change:
		return want_text(l);
	    case command::substitute: case command::repeat_sub:
	    case command::repeat_sub_flags:
		return substitute(l);
	    case command::global: case command::vglobal:
		return global(l);
	    case command::sort:
		return sort(l);
	    case command::retab:
		return retab(l);
	    case command::read:
		return read(l);
	    case command::write: case command::write_quit:
	    case command::exit: case command::update:
		return write(l);
	    case command::saveas:
		return saveas(l);
	    default:
		return fail(std::string("'") + command_name(l.code)
			    + "' is not an editor command yet");
	}
    }

    bool take_text(const std::vector<std::string> &body)
    {
	const session::pending_text p = s.pending;
	s.pending = session::pending_text();
	if ( !begin_change() )
	    return false;
	size_t after = p.last;
	if ( p.code == command::insert )
	    after = p.last ? p.last - 1 : 0;
	else if ( p.code == command::change )
	{
	    erase_lines(p.first, p.last);
	    after = p.first - 1;
	}
	insert_after(after, body);
	size_t to = after + body.size();
	const size_t n = lines();
	if ( to == 0 && n > 0 )
	    to = 1;
	set_dot(to > n ? n : to);
	return true;
    }
};

session::session(madc::hub::world &world)
    : w(world), cur(0), alternate(0), next_number(1),
      dir(madc::detail::resolve_real_path("."))
{
    if ( dir.empty() )
	dir = ".";
}

size_t session::number_of(entity_id doc)
{
    std::map<entity_id, size_t>::iterator it = numbers.find(doc);
    if ( it != numbers.end() )
	return it->second;
    numbers[doc] = next_number;
    return next_number++;
}

session::~session()
{
    // The anchors are the documents'; a document outlives its session's
    // view of it only as long as the world keeps it.
    mutation_context mc(w);
    for ( auto &b : buffers )
    {
	if ( !w.has_doc(b.first) )
	    continue;
	if ( b.second.dot )
	    mc.anchor_remove(b.first, b.second.dot);
	for ( auto &m : b.second.marks )
	    mc.anchor_remove(b.first, m.second);
    }
}

bool session::use(entity_id doc)
{
    if ( !doc || !w.has_doc(doc) )
	return false;
    if ( cur && cur != doc && w.has_doc(cur) )
	alternate = cur;
    cur = doc;
    number_of(doc);
    buffer_state &st = buffers[doc];
    if ( !st.dot )
    {
	const text_buffer *b = w.text_of(doc);
	size_t n = b ? b->line_count() : 0;
	size_t off = 0, len = 0;
	if ( n )
	    b->line_span(n, off, len);
	mutation_context mc(w);
	st.dot = mc.anchor_add(doc, n ? off : 0, false);
	// The buffer's indent (design decision 12), as a buffer opens: the
	// unit its first lines show, else its language's — a tab keeps
	// 'noexpandtab', spaces set 'expandtab' and 'shiftwidth'.
	size_t head = 0, head_len = 0;
	if ( n )
	    b->line_span(n < 1000 ? n : 1000, head, head_len);
	std::string unit = madc::indent_detect(n ? b->slice(0, head + head_len)
						 : std::string());
	const madc::hub::document *d = w.doc_of(doc);
	if ( unit.empty() && d )
	    unit = madc::file_kind_indent_default(d->kind());
	if ( unit == "\t" )
	{
	    opts.expandtab = false;
	    opts.shiftwidth = opts.tabstop;
	}
	else if ( !unit.empty() )
	{
	    opts.expandtab = true;
	    opts.shiftwidth = unit.size();
	}
    }
    return true;
}

size_t session::current_line() const
{
    const text_buffer *b = cur ? w.text_of(cur) : (const text_buffer *)0;
    const size_t n = b ? b->line_count() : 0;
    if ( n == 0 )
	return 0;
    std::map<entity_id, buffer_state>::const_iterator it = buffers.find(cur);
    const anchor *a = it == buffers.end() ? (const anchor *)0
			: w.anchor_of(cur, it->second.dot);
    if ( !a )
	return n;
    size_t l = b->line_of(a->off);
    return l > n ? n : l;
}

bool session::set_current_line(size_t n)
{
    const text_buffer *b = cur ? w.text_of(cur) : (const text_buffer *)0;
    size_t off = 0, len = 0;
    if ( !b || n == 0 || n > b->line_count() || !b->line_span(n, off, len) )
	return false;
    buffer_state &st = buffers[cur];
    mutation_context mc(w);
    if ( !st.dot || !mc.anchor_move(cur, st.dot, off) )
	st.dot = mc.anchor_add(cur, off, false);
    return true;
}

outcome session::run(const std::string &text)
{
    outcome out;
    if ( wants_text() )
    {
	out.ok = false;
	out.error = "text is pending: end it with a line holding only '.'";
	return out;
    }
    line l;
    std::string error;
    if ( !parse(text, l, error) )
    {
	out.ok = false;
	out.error = error;
	return out;
    }
    executor x(*this, out);
    x.execute(l);
    return out;
}

outcome session::text(const std::vector<std::string> &body)
{
    outcome out;
    if ( !wants_text() )
    {
	out.ok = false;
	out.error = "no command is waiting for text";
	return out;
    }
    executor x(*this, out);
    x.take_text(body);
    return out;
}

} // namespace ex

// ---- the dialect face (include/madc_ex_session.h) ----------------------

namespace {

value outcome_value(const ex::outcome &o, ex::command code)
{
    std::map<std::string, value> f;
    f["ok"] = value(o.ok);
    f["shown"] = value(o.shown);
    f["error"] = value(o.error);
    f["wants_text"] = value(o.wants_text);
    f["quit"] = value(o.quit);
    f["command"] = value((int64_t)code);
    return value::make_object(f);
}

ex::outcome refused(const char *why)
{
    ex::outcome o;
    o.ok = false;
    o.error = why;
    return o;
}

} // namespace

ex_session::ex_session(int64_t w) : _s(0)
{
    if ( hub::world *world = madc_ui_world(w) )
	_s = new ex::session(*world);
}

ex_session::~ex_session()
{
    delete _s;
}

bool ex_session::use(int64_t doc)
{
    return _s && _s->use((hub::entity_id)doc);
}

int64_t ex_session::current()
{
    return _s ? (int64_t)_s->current() : 0;
}

bool ex_session::set_line(int64_t line)
{
    return _s && line > 0 && _s->set_current_line((size_t)line);
}

int64_t ex_session::line()
{
    return _s ? (int64_t)_s->current_line() : 0;
}

value &ex_session::parse(value &out, const char *text)
{
    ex::line l;
    std::string error;
    const bool ok = ex::parse(text ? text : "", l, error);
    std::map<std::string, value> f;
    f["ok"] = value(ok);
    f["error"] = value(error);
    f["command"] = value((int64_t)(ok ? l.code : ex::command::none));
    f["force"] = value(ok && l.force);
    f["argument"] = value(ok ? l.argument : std::string());
    out = value::make_object(f);
    return out;
}

value &ex_session::run(value &out, const char *text)
{
    const std::string t = text ? text : "";
    ex::line l;
    std::string error;
    const ex::command code = ex::parse(t, l, error) ? l.code : ex::command::none;
    out = outcome_value(_s ? _s->run(t) : refused("no world"), code);
    return out;
}

value &ex_session::text(value &out, value &lines)
{
    std::vector<std::string> body;
    if ( lines.is_array() )
	for ( const value &v : lines.as_array() )
	    body.push_back(v.is_string() ? v.as_string() : std::string());
    out = outcome_value(_s ? _s->text(body) : refused("no world"), ex::command::none);
    return out;
}

bool ex_session::wants_text()
{
    return _s && _s->wants_text();
}

} // namespace madc
