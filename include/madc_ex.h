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
#include <map>
#include <string>
#include <vector>

#include "madcdis/document.h"	// entity_id, anchor_id
#include "madc/bits/ex_commands"	// ex_command: the one enum text

namespace madc {
namespace hub { class world; }

namespace ex {

// Every command the core knows (D24's list): the one enum text is
// <bits/ex_commands>, which a dialect host reads too.
typedef madc::ex_command command;

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
    size_t      count;		// that count; ret: the new tabstop (0: none)
    int         levels;		// > / <: one per `>` or `<` typed
    std::string pattern;	// s, g, v, sor: the pattern (empty: the last one)
    bool        has_pattern;	// sor: a /pattern/ was typed
    std::string replacement;	// s
    std::string flags;		// s: `g` `i` `I` `n` `e`, and `&` (keep the
				// last); sor: `i` `u` `r` and one of `n x o b f`;
				// w: `>>` (append)
    std::string subcommand;	// g, v: the command run on each line
    bool        shell_form;	// :r !cmd, :w !cmd — the argument is a command

    line()
	: code(command::none), force(false), has_count(false), count(0),
	  levels(0), has_pattern(false), shell_form(false) {}
};

// A typed command name (`s`, `sub`, `substitute`, `bn`) as its code: the
// row whose full name starts with `name` at least as long as its shortest
// abbreviation (vim's rule). False when no row covers it.
bool command_of(const std::string &name, command &code);

// Parse one command line. False when it is refused; `error` then says why
// (`not an editor command: 'xyz'`, `trailing characters: 'x'`).
bool parse(const std::string &text, line &out, std::string &error);

// ------------------------------------------------------------------ session
// The ex core's state over one hub world (design §2.3): the current buffer,
// each buffer's current line and marks as document ANCHORS (they follow
// every edit, an ex edit or another client's), the last search pattern and
// the last substitute, the options, and a pending a / i / c waiting for its
// text. Every edit goes through the world's mutation path
// (madcdis/verbs.h mutation_context): it publishes, shifts anchors, and is
// ONE undo group per command line.

// What one command line did: the lines it shows (each ending in '\n'), or
// why it was refused; whether it now waits for text (a / i / c — the host
// feeds lines until one holding only `.`) or ends the host (q / wq / x).
struct outcome
{
    bool        ok;
    std::string shown;
    std::string error;
    bool        wants_text;
    bool        quit;

    outcome() : ok(true), wants_text(false), quit(false) {}
};

// :set's options.
struct options
{
    bool   number;		// show line numbers when printing
    size_t tabstop;
    size_t shiftwidth;
    bool   expandtab;

    options() : number(false), tabstop(8), shiftwidth(8), expandtab(false) {}
};

class session
{
public:
    explicit session(madc::hub::world &w);
    ~session();

    // The current buffer (0: none), and making a world's document current
    // (the one before it becomes the alternate, `#`); a buffer seen for the
    // first time starts at its last line (ex's rule after a file is read).
    madc::hub::entity_id current() const { return cur; }
    bool use(madc::hub::entity_id doc);
    // The session's working directory (:cd, :pwd; the process's at the
    // start): :e :r :w :sav and the shell commands resolve a relative path
    // against it. The process's own directory never changes.
    const std::string &directory() const { return dir; }
    // The current buffer's current line (0: no buffer, or an empty one),
    // and setting it (a visual host's caret line, so `.` is where the
    // caret is): false when `n` is not one of the buffer's lines.
    size_t current_line() const;
    bool set_current_line(size_t n);

    // Run one command line (the text after colon mode's `:`).
    outcome run(const std::string &text);
    // The text a pending a / i / c waits for: `lines` without their line
    // breaks and without the closing `.`.
    outcome text(const std::vector<std::string> &lines);
    bool wants_text() const { return pending.code != command::none; }

    options &settings() { return opts; }

private:
    struct buffer_state
    {
	madc::hub::anchor_id dot;
	std::map<char, madc::hub::anchor_id> marks;
	buffer_state() : dot(0) {}
    };
    struct pending_text
    {
	command code;
	size_t  first;
	size_t  last;
	pending_text() : code(command::none), first(0), last(0) {}
    };
    // The last :s, for `&` / `&&` / a bare `:s`.
    struct last_substitute
    {
	bool        set;
	std::string pattern;
	std::string replacement;
	std::string flags;
	last_substitute() : set(false) {}
    };

    madc::hub::world &w;
    madc::hub::entity_id cur;
    madc::hub::entity_id alternate;
    std::map<madc::hub::entity_id, buffer_state> buffers;
    // :ls's buffer numbers, given in the order the world's documents are
    // first seen; a number is never reused.
    std::map<madc::hub::entity_id, size_t> numbers;
    size_t next_number;
    std::string dir;
    std::string previous_dir;	// :cd -
    std::string last_pattern;
    last_substitute last_sub;
    options opts;
    pending_text pending;

    // `doc`'s :ls number, given now when it has none.
    size_t number_of(madc::hub::entity_id doc);

    session(const session &);
    session &operator=(const session &);

    friend struct executor;
};

} // namespace ex
} // namespace madc

#endif
