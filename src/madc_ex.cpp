// madc_ex.cpp — the ex core (split arc Phase 1B.6; design
// docs/plans/2026-10-10-ex-core-design.md; header include/madc_ex.h).
//
// The command line's grammar is vim's ex, line-oriented (D24): addresses
// and ranges, a command name resolved once through ONE table to its code,
// and each command's operands. Oracle: vim's ex mode (`vim -e -s`) on the
// same line.
//
// THREAD-SAFETY CONTRACT: see madc_ex.h — pure functions over their input,
// one immutable table.

#include <cctype>
#include <cstring>
#include <string>

#include "madc_ex.h"

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

} // namespace ex
} // namespace madc
