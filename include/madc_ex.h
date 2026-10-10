#ifndef __MADC_EX_H
#define __MADC_EX_H 1

// madc_ex.h — the ex core (split arc Phase 1B.6; design
// docs/plans/2026-10-10-ex-core-design.md): madc's colon mode, D24's
// line-oriented ed / ex / vim commands over the documents of a hub world.
// madc is ex; madcide is the visual part and registers only what needs its
// views. This header is the command LINE's grammar: an address, a range, a
// command name resolved ONCE to its code (enum-over-strings), and each
// command's own operands — parsed, never yet applied.
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): the parser is a
// pure function over its input; the command table is immutable. No global
// state.

#include <cstddef>
#include <string>

namespace madc {
namespace ex {

// Every command the core knows (D24's list). A typed name becomes one of
// these once, through the one table (src/madc_ex.cpp); `none` is a line
// with no command — a bare address or an empty line.
enum class command : unsigned char
{
    none,
    print, number, list, line_number,		// p  nu/#  l  =
    append, insert, change, del, join,		// a  i  c  d  j
    move, copy, shift_right, shift_left,	// m  t/co  >  <
    undo, redo,					// u  red
    substitute, repeat_sub, repeat_sub_flags,	// s  &  &&
    global, vglobal,				// g  v
    sort, retab,				// sor  ret
    mark, marks,				// ma/k  marks
    read, write, write_quit, exit, update, saveas, edit, quit,
						// r w wq x up sav e q
    buffers, buffer, bnext, bprevious,		// ls  b  bn  bp
    cd, pwd, set, help, shell			// cd pw se h  !
};

// The command's name as madc spells it (its full ex name), for messages.
const char *command_name(command c);

// One address as typed: a base line and the offsets after it
// (`.+3-1`, `/re/+2`). Resolving it against a buffer is the session's.
struct address
{
    enum class base : unsigned char
    {
	none,		// only offsets: relative to the current line
	line,		// N
	current,	// .
	last,		// $
	search_forward,	// /re/  (an empty pattern: the last one used)
	search_backward,// ?re?
	mark		// 'x
    };
    base        kind;
    size_t      line;		// N
    std::string pattern;	// the search's pattern, its delimiters removed
    char        mark_name;	// 'x
    long        offset;		// the sum of the +N / -N after the base

    address() : kind(base::none), line(0), mark_name(0), offset(0) {}
};

// A range: no address, one, or two (`N,M`; `%` is `1,$`). `;` makes the
// first address the current line before the second is resolved.
struct range
{
    int     count;		// 0, 1 or 2
    address first;
    address second;
    bool    from_current;	// the separator was `;`

    range() : count(0), from_current(false) {}
};

// One parsed command line (the text after colon mode's `:`).
struct line
{
    range       where;
    command     code;
    bool        force;		// `!` right after the name (q!, e!, w!)
    std::string argument;	// what follows the name, blanks before it dropped
    // Operands some commands carry, parsed here:
    address     destination;	// m / t / co: the line the text goes after
    bool        has_count;	// p nu l d j > < s: a trailing count
    size_t      count;
    int         levels;		// > / <: one per `>` or `<` typed
    std::string pattern;	// s, g, v: the pattern (empty: the last one)
    std::string replacement;	// s
    std::string flags;		// s: `g` `i` `I` `n`, and `&` (keep the last)
    std::string subcommand;	// g, v: the command run on each line
    bool        shell_form;	// :r !cmd, :w !cmd — the argument is a command

    line()
	: code(command::none), force(false), has_count(false), count(0),
	  levels(0), shell_form(false) {}
};

// A typed command name (`s`, `sub`, `substitute`, `bn`) as its code: the
// row whose full name starts with `name` at least as long as its shortest
// abbreviation (vim's rule). False when no row covers it.
bool command_of(const std::string &name, command &code);

// Parse one command line. False when it is refused; `error` then says why
// (`not an editor command: 'xyz'`, `trailing characters: 'x'`).
bool parse(const std::string &text, line &out, std::string &error);

} // namespace ex
} // namespace madc

#endif
