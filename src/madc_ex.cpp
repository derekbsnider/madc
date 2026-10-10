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

#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "madc_ex.h"
#include "madc_regex.h"		// /re/, ?re? — madc's one regex engine
#include "madcdis/hub.h"
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
	    // e, sav, cd, b, se, h, sor, ret, marks: the rest as typed.
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

    executor(session &ss, outcome &o) : s(ss), out(o) {}

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
	madc::regex re(pattern);
	if ( !re.ok() )
	    return fail("invalid pattern '" + pattern + "': " + re.error());
	s.last_pattern = pattern;
	const size_t n = lines();
	const size_t base = from == 0 ? (forward ? 0 : n + 1) : from;
	for ( size_t i = 1; i <= n; ++i )
	{
	    // Wrapping, as vim's 'wrapscan': forward from the line after
	    // `from` to `from` itself, backward from the line before it.
	    size_t l = forward ? (base + i - 1) % n + 1
			       : (base - 1 + n - i) % n + 1;
	    if ( re.search(line_text(l)) )
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
    bool begin_change()
    {
	const madc::hub::document *d = s.w.doc_of(s.cur);
	if ( d && d->read_only() )
	    return fail("the buffer is read-only");
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
    }
    void replace_line(size_t n, const std::string &with)
    {
	const text_buffer *b = text();
	size_t off = 0, len = 0;
	if ( !b->line_span(n, off, len) )
	    return;
	mutation_context mc(s.w);
	mc.text_replace(s.cur, off, len, with);
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

session::session(madc::hub::world &world) : w(world), cur(0) {}

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
    cur = doc;
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
} // namespace madc
