#ifndef __MADCDIS_TUI_MODEL_H
#define __MADCDIS_TUI_MODEL_H 1

// madcdis/tui_model.h — the level-1 renderer's MODEL (Track 7.2 R5):
// everything an addressable-character-grid frontend does that is not
// terminal I/O, dependency-free and unit-testable. The uinode tree in,
// a cell grid + semantic events out:
//
//   - layout: the same semantic tree the level-0 renderer linearizes,
//     composed onto a rows×cols grid (heading/status bars, wrapped
//     content, a flexible `edit` window, a `choice` menu bar);
//   - focus and selection: choice options are NAVIGABLE here (the same
//     tree line mode numbers — design success criterion 4); focus cycles
//     across choice/edit nodes;
//   - the input adapter: raw terminal bytes → keys (tui_keyparse: CSI/SS3
//     escape parsing with an explicit flush for the bare-ESC pause) and
//     keys → SEMANTIC events, coalescing printable runs into one text
//     event (design §7.5 — five key events never become five domain
//     transactions);
//   - differential support: dirty-row comparison between two grids.
//
// The TARGET (a provider behind the ui:: session surface — the hand-
// rolled VT100/xterm one in src/ui_term.cpp, owner-decided 2026-08-25
// over vendoring ncurses/termbox2/notcurses) only moves bytes: raw mode,
// escape emission, key reads. Richer providers plug in behind the same
// seam without touching this model.
//
// PRESENTATION STATE LIVES HERE (design §7.2): scroll windows, horizontal
// shift, focus slot, menu selection. Interaction state (caret, selection,
// search) is the APPLICATION's, carried on the tree as `edit`-node hints
// (byte offsets: "caret", "sel_start", "sel_end"); domain state never
// enters. Byte-oriented in this pilot: multi-byte (UTF-8) glyphs occupy
// one cell per byte — a named residue, not a contract.
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): a tui_model is
// a plain object confined to the thread that composes and applies keys;
// two models never share state.

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "madcdis/uinode.h"
#include "madcdis/render_text.h"	// wrap_text — the one wrap owner

namespace madc {
namespace hub {

// ------------------------------------------------------------------ the grid
// The render STYLE (AST-2; owner: VT-102's ANSI colours, JOE parity).
// What JOE's syntax vocabulary can say: the classic attributes plus an
// 8-colour foreground/background — 16 effective foreground colours via
// bold-as-bright, the VT-102/16-colour model (no aixterm 90–97).
// A THEME (app data) maps classification names to style SPECS;
// tui_attr_of below is the one spec parser at the value boundary; the
// VT100 target owns style->SGR. 256/true-colour is a named later seat.
struct tui_attr
{
    enum : unsigned char
    {
	BOLD	  = 1,
	DIM	  = 2,
	ITALIC	  = 4,
	UNDERLINE = 8,
	BLINK	  = 16,
	INVERSE	  = 32
    };
    unsigned char fg;		// 0 = default, 1..8 = black..white (ANSI+1)
    unsigned char bg;		// same domain
    unsigned char flags;	// the attribute bits above
    tui_attr() : fg(0), bg(0), flags(0) {}
    bool operator==(const tui_attr &o) const
	{ return fg == o.fg && bg == o.bg && flags == o.flags; }
    bool operator!=(const tui_attr &o) const { return !(*this == o); }
    bool is_normal() const { return fg == 0 && bg == 0 && flags == 0; }
    // Pure inverse — the pre-colour renderer's one non-normal style; the
    // VT100 target keeps its historical \x1b[7m spelling for it.
    bool is_reverse() const { return fg == 0 && bg == 0 && flags == INVERSE; }
    static tui_attr normal() { return tui_attr(); }
    static tui_attr reverse()
	{ tui_attr a; a.flags = INVERSE; return a; }
};

// THE style-spec parser (JOE's vocabulary, one table): space-separated
// words — attributes `bold dim italic underline blink inverse` (JOE's
// `reverse` accepted as a synonym), a foreground colour word
// `black red green yellow blue magenta cyan white`, a background
// `bg_<colour>`, and `normal` (alone) for the default style. False =
// any unknown word (the WHOLE spec is refused — themes fail loud).
inline bool tui_attr_of(const std::string &spec, tui_attr &out)
{
    static const char *const colours[8] = {
	"black", "red", "green", "yellow",
	"blue", "magenta", "cyan", "white"
    };
    tui_attr a;
    bool any = false;
    size_t i = 0;
    while ( i < spec.size() )
    {
	while ( i < spec.size() && (spec[i] == ' ' || spec[i] == '\t') )
	    ++i;
	size_t start = i;
	while ( i < spec.size() && spec[i] != ' ' && spec[i] != '\t' )
	    ++i;
	if ( i == start )
	    break;
	std::string w = spec.substr(start, i - start);
	if ( w == "normal" )	    { any = true; continue; }
	if ( w == "bold" )	    { a.flags |= tui_attr::BOLD; any = true; continue; }
	if ( w == "dim" )	    { a.flags |= tui_attr::DIM; any = true; continue; }
	if ( w == "italic" )	    { a.flags |= tui_attr::ITALIC; any = true; continue; }
	if ( w == "underline" )	    { a.flags |= tui_attr::UNDERLINE; any = true; continue; }
	if ( w == "blink" )	    { a.flags |= tui_attr::BLINK; any = true; continue; }
	if ( w == "inverse" || w == "reverse" )
				    { a.flags |= tui_attr::INVERSE; any = true; continue; }
	bool matched = false;
	for ( int c = 0; c < 8 && !matched; ++c )
	{
	    if ( w == colours[c] )
	    {
		a.fg = (unsigned char)(c + 1);
		matched = true;
	    }
	    else if ( w.compare(0, 3, "bg_") == 0
		   && w.compare(3, std::string::npos, colours[c]) == 0 )
	    {
		a.bg = (unsigned char)(c + 1);
		matched = true;
	    }
	}
	if ( !matched )
	    return false;
	any = true;
    }
    if ( !any )
	return false;
    out = a;
    return true;
}

struct tui_cell
{
    char     ch;
    tui_attr attr;
    tui_cell() : ch(' '), attr(tui_attr::normal()) {}
    bool operator==(const tui_cell &o) const
	{ return ch == o.ch && attr == o.attr; }
    bool operator!=(const tui_cell &o) const { return !(*this == o); }
};

struct tui_grid
{
    size_t rows, cols;
    std::vector<tui_cell> cells;
    size_t cursor_row, cursor_col;	// the physical cursor (an edit caret)
    bool   cursor_visible;

    tui_grid() : rows(0), cols(0), cursor_row(0), cursor_col(0),
		 cursor_visible(false) {}

    void resize(size_t r, size_t c)
    {
	rows = r;
	cols = c;
	cells.assign(r * c, tui_cell());
	cursor_row = cursor_col = 0;
	cursor_visible = false;
    }
    tui_cell &at(size_t r, size_t c) { return cells[r * cols + c]; }
    const tui_cell &at(size_t r, size_t c) const { return cells[r * cols + c]; }

    // Clipped text write; never wraps. THE CELL INVARIANT: a cell holds one
    // printable byte occupying exactly one terminal column — a control byte
    // in a cell desynchronizes grid columns from screen columns (a raw tab
    // MOVES the terminal cursor without erasing the skipped columns: stale
    // fragments + doubled glyphs while scrolling, the IDE-9c defect). Tab
    // expansion is the document projection's job (paint_edit's display map);
    // here every control byte renders as a visible '?'. Bytes >= 0x80 pass
    // through (UTF-8 renders byte-per-cell today; the multi-column glyph
    // model is the doc-lens display-map seat).
    void put(size_t r, size_t c, const std::string &text,
	     tui_attr attr = tui_attr::normal())
    {
	if ( r >= rows )
	    return;
	for ( size_t i = 0; i < text.size() && c + i < cols; ++i )
	{
	    tui_cell &cell = at(r, c + i);
	    char b = text[i];
	    cell.ch = (unsigned char)b < 0x20 || b == 0x7f ? '?' : b;
	    cell.attr = attr;
	}
    }
    void fill_attr(size_t r, size_t c, size_t len, tui_attr attr)
    {
	if ( r >= rows )
	    return;
	for ( size_t i = 0; i < len && c + i < cols; ++i )
	    at(r, c + i).attr = attr;
    }
    // The row as text, right-trimmed — the unit batteries' view.
    std::string row_text(size_t r) const
    {
	std::string out;
	if ( r >= rows )
	    return out;
	for ( size_t c = 0; c < cols; ++c )
	    out += at(r, c).ch;
	size_t end = out.find_last_not_of(' ');
	return end == std::string::npos ? std::string() : out.substr(0, end + 1);
    }
};

// Rows differing between two grids — what a target repaints. A dimension
// change dirties everything.
inline std::vector<size_t> tui_dirty_rows(const tui_grid &prev,
					  const tui_grid &next)
{
    std::vector<size_t> out;
    if ( prev.rows != next.rows || prev.cols != next.cols )
    {
	for ( size_t r = 0; r < next.rows; ++r )
	    out.push_back(r);
	return out;
    }
    for ( size_t r = 0; r < next.rows; ++r )
	for ( size_t c = 0; c < next.cols; ++c )
	    if ( prev.at(r, c) != next.at(r, c) )
	    {
		out.push_back(r);
		break;
	    }
    return out;
}

// ------------------------------------------------------------------- the keys
enum class tui_key : unsigned char
{
    none = 0,
    ch,		// printable byte in `ch`
    ctrl,	// control chord; `ch` = the lowercase letter (^S -> 's')
		// or one of the four punctuation controls 0x1c..0x1f
		// ('\\' ']' '^' '_' — JOE's ^_ undo / ^^ redo live here)
    enter, tab, backspace, esc,
    up, down, left, right,
    home, end, pgup, pgdn, del, ins,
    resize	// synthesized by the target on a size change
};

struct tui_keyev
{
    tui_key kind;
    char    ch;
    tui_keyev() : kind(tui_key::none), ch(0) {}
    explicit tui_keyev(tui_key k, char c = 0) : kind(k), ch(c) {}
};

// The ONE key-spelling owner, both directions (ids/enums inside, names at
// the value boundary): what a tui_event's `key` field carries to the
// script, and what a bindings table's sequences are written in. Control
// chords spell "^"+letter; a printable spells as itself ("space" for the
// blank, which cannot stand alone in a space-separated sequence).
inline std::string tui_key_name(const tui_keyev &k)
{
    switch ( k.kind )
    {
	case tui_key::ch:
	    return k.ch == ' ' ? std::string("space") : std::string(1, k.ch);
	case tui_key::ctrl:	 return std::string("^") + k.ch;
	case tui_key::enter:	 return "enter";
	case tui_key::tab:	 return "tab";
	case tui_key::backspace: return "backspace";
	case tui_key::esc:	 return "esc";
	case tui_key::up:	 return "up";
	case tui_key::down:	 return "down";
	case tui_key::left:	 return "left";
	case tui_key::right:	 return "right";
	case tui_key::home:	 return "home";
	case tui_key::end:	 return "end";
	case tui_key::pgup:	 return "pgup";
	case tui_key::pgdn:	 return "pgdn";
	case tui_key::del:	 return "del";
	case tui_key::ins:	 return "ins";
	default:		 return "";
    }
}

// Spelling -> key. Generous on input (an upper-case letter after "^"
// lowers), canonical on output via tui_key_name. False = not a spelling.
inline bool tui_key_from_name(const std::string &name, tui_keyev &out)
{
    if ( name.empty() )
	return false;
    if ( name.size() == 2 && name[0] == '^' )
    {
	char c = name[1];
	if ( c >= 'A' && c <= 'Z' )
	    c = (char)(c - 'A' + 'a');
	if ( (c < 'a' || c > 'z') && c != '\\' && c != ']' && c != '^'
		&& c != '_' )
	    return false;
	out = tui_keyev(tui_key::ctrl, c);
	return true;
    }
    if ( name.size() == 1 && name[0] >= 0x20 && name[0] <= 0x7e )
    {
	out = tui_keyev(tui_key::ch, name[0]);
	return true;
    }
    static const struct { const char *n; tui_key k; } named[] = {
	{ "space", tui_key::ch }, { "enter", tui_key::enter },
	{ "tab", tui_key::tab }, { "backspace", tui_key::backspace },
	{ "esc", tui_key::esc }, { "up", tui_key::up },
	{ "down", tui_key::down }, { "left", tui_key::left },
	{ "right", tui_key::right }, { "home", tui_key::home },
	{ "end", tui_key::end }, { "pgup", tui_key::pgup },
	{ "pgdn", tui_key::pgdn }, { "del", tui_key::del },
	{ "ins", tui_key::ins },
    };
    for ( size_t i = 0; i < sizeof(named) / sizeof(named[0]); ++i )
	if ( name == named[i].n )
	{
	    out = tui_keyev(named[i].k, named[i].k == tui_key::ch ? ' ' : 0);
	    return true;
	}
    return false;
}

// ------------------------------------------------------------- the bindings
// Key sequences -> action names: DATA, installed per profile (owner
// 2026-08-25 — the JOE/WordStar ^K-chord ruling; a profile swap is a new
// table, never a second hardcoded map). A sequence is space-separated key
// spellings ("^k s"), any length. Validation is loud and whole-table at
// finalize(): a sequence must START with a non-printable key (a printable
// head would swallow typing), and no bound sequence may be a proper
// prefix of another (deterministic resolution — JOE's ^K is only ever a
// prefix). Canonical spellings are the map keys, so lookups and the seq
// reported on events agree byte-for-byte.
class tui_bindings
{
    std::map<std::string, std::string> _actions;
    std::set<std::string> _prefixes;

public:
    // SEQUENCE spelling: tui_key_name with printable LETTERS lowered —
    // chords are letter-case-insensitive (JOE's ^K S == ^K s convention;
    // the shift state of a chord continuation never distinguishes
    // bindings). Key EVENTS keep tui_key_name's exact spelling.
    static std::string seq_spelling(const tui_keyev &k)
    {
	if ( k.kind == tui_key::ch && k.ch >= 'A' && k.ch <= 'Z' )
	    return std::string(1, (char)(k.ch - 'A' + 'a'));
	return tui_key_name(k);
    }

    // CONTINUATION spelling (every key after the head): JOE's other
    // chord convention — the ctrl state of a continuation never
    // distinguishes bindings either (^K ^Z == ^K Z; users keep ctrl
    // held), so a ctrl+letter continuation spells as the bare letter.
    // Ctrl+punctuation (^_ ^^ ^] ^\) has no letter form and stays
    // itself. bind() canonicalization and the model's pending-chord
    // extension both ride this — one owner, both directions.
    static std::string cont_spelling(const tui_keyev &k)
    {
	if ( k.kind == tui_key::ctrl && k.ch >= 'a' && k.ch <= 'z' )
	    return std::string(1, k.ch);
	return seq_spelling(k);
    }

    bool empty() const { return _actions.empty(); }
    void clear() { _actions.clear(); _prefixes.clear(); }

    // Parse + canonicalize one sequence; false (table untouched) on a
    // spelling that is not a key.
    bool bind(const std::string &seq, const std::string &action)
    {
	std::string canon;
	size_t i = 0;
	while ( i < seq.size() )
	{
	    while ( i < seq.size() && seq[i] == ' ' )
		++i;
	    size_t j = i;
	    while ( j < seq.size() && seq[j] != ' ' )
		++j;
	    if ( j == i )
		break;
	    tui_keyev k;
	    if ( !tui_key_from_name(seq.substr(i, j - i), k) )
		return false;
	    if ( canon.empty() )
		canon += seq_spelling(k);
	    else
	    {
		canon += ' ';
		canon += cont_spelling(k);
	    }
	    i = j;
	}
	if ( canon.empty() )
	    return false;
	_actions[canon] = action;
	return true;
    }

    // Whole-table validation + the prefix set. False leaves the table
    // unusable by contract; `err` names the offending sequence.
    bool finalize(std::string &err)
    {
	_prefixes.clear();
	for ( std::map<std::string, std::string>::const_iterator it
		= _actions.begin(); it != _actions.end(); ++it )
	{
	    const std::string &seq = it->first;
	    tui_keyev head;
	    tui_key_from_name(seq.substr(0, seq.find(' ')), head);
	    if ( head.kind == tui_key::ch )
	    {
		err = "printable-headed sequence: " + seq;
		return false;
	    }
	    for ( size_t sp = seq.find(' '); sp != std::string::npos;
		  sp = seq.find(' ', sp + 1) )
	    {
		std::string prefix = seq.substr(0, sp);
		if ( _actions.count(prefix) )
		{
		    err = "sequence shadows a shorter binding: " + seq;
		    return false;
		}
		_prefixes.insert(prefix);
	    }
	}
	return true;
    }

    bool bound(const std::string &canon_seq) const
	{ return _actions.count(canon_seq) != 0; }
    bool prefix(const std::string &canon_seq) const
	{ return _prefixes.count(canon_seq) != 0; }
    const std::string &action_of(const std::string &canon_seq) const
    {
	static const std::string none;
	std::map<std::string, std::string>::const_iterator it
	    = _actions.find(canon_seq);
	return it == _actions.end() ? none : it->second;
    }
};

// Raw terminal bytes -> keys: the escape-sequence state machine (CSI and
// SS3 forms of the VT100/xterm family; the shapes every terminal library
// parses — cross-checked against termbox2's and ncurses's tables). A bare
// ESC is ambiguous until the input pauses: the TARGET calls flush() when
// its read times out after an ESC, resolving it to the esc key. Modifier
// parameters on arrows ("1;2A") resolve to the unmodified key in this
// pilot.
class tui_keyparse
{
    enum class state : unsigned char { normal, esc, csi, ss3 };
    state _st;
    std::string _params;

    static void emit(std::vector<tui_keyev> &out, tui_key k, char c = 0)
    {
	out.push_back(tui_keyev(k, c));
    }
    static void resolve_csi(const std::string &params, char final_byte,
			    std::vector<tui_keyev> &out)
    {
	switch ( final_byte )
	{
	    case 'A': emit(out, tui_key::up); return;
	    case 'B': emit(out, tui_key::down); return;
	    case 'C': emit(out, tui_key::right); return;
	    case 'D': emit(out, tui_key::left); return;
	    case 'H': emit(out, tui_key::home); return;
	    case 'F': emit(out, tui_key::end); return;
	    case '~':
		switch ( params.empty() ? 0 : atoi(params.c_str()) )
		{
		    case 1: case 7: emit(out, tui_key::home); return;
		    case 4: case 8: emit(out, tui_key::end); return;
		    case 2: emit(out, tui_key::ins); return;
		    case 3: emit(out, tui_key::del); return;
		    case 5: emit(out, tui_key::pgup); return;
		    case 6: emit(out, tui_key::pgdn); return;
		    default: return;	// unrecognized: dropped
		}
	    default: return;		// unrecognized final: dropped
	}
    }
    void feed_byte(unsigned char b, std::vector<tui_keyev> &out)
    {
	switch ( _st )
	{
	    case state::esc:
		if ( b == '[' )
		{
		    _st = state::csi;
		    _params.clear();
		    return;
		}
		if ( b == 'O' )
		{
		    _st = state::ss3;
		    return;
		}
		// ESC followed by an ordinary byte: the ESC stands alone
		// (alt-chords are a deferred refinement) and the byte is
		// reprocessed normally.
		emit(out, tui_key::esc);
		_st = state::normal;
		feed_byte(b, out);
		return;
	    case state::csi:
		if ( b >= 0x40 && b <= 0x7e )
		{
		    resolve_csi(_params, (char)b, out);
		    _st = state::normal;
		}
		else if ( _params.size() < 16 )
		    _params += (char)b;
		else
		    _st = state::normal;	// runaway sequence: dropped
		return;
	    case state::ss3:
		switch ( b )
		{
		    case 'A': emit(out, tui_key::up); break;
		    case 'B': emit(out, tui_key::down); break;
		    case 'C': emit(out, tui_key::right); break;
		    case 'D': emit(out, tui_key::left); break;
		    case 'H': emit(out, tui_key::home); break;
		    case 'F': emit(out, tui_key::end); break;
		    default: break;		// unrecognized: dropped
		}
		_st = state::normal;
		return;
	    case state::normal:
	    default:
		break;
	}
	if ( b == 0x1b )
	    _st = state::esc;
	else if ( b == '\r' || b == '\n' )
	    emit(out, tui_key::enter);
	else if ( b == '\t' )
	    emit(out, tui_key::tab);
	else if ( b == 0x7f || b == 0x08 )
	    emit(out, tui_key::backspace);
	else if ( b >= 0x01 && b <= 0x1a )
	    emit(out, tui_key::ctrl, (char)('a' + b - 1));
	else if ( b >= 0x1c && b <= 0x1f )
	    emit(out, tui_key::ctrl, (char)(b + 0x40));	// ^\ ^] ^^ ^_
	else if ( b >= 0x20 && b <= 0x7e )
	    emit(out, tui_key::ch, (char)b);
	// 0x00, >=0x80: dropped (byte-oriented pilot; UTF-8 glyph
	// handling is the named residue).
    }

public:
    tui_keyparse() : _st(state::normal) {}

    void feed(const char *bytes, size_t n, std::vector<tui_keyev> &out)
    {
	for ( size_t i = 0; i < n; ++i )
	    feed_byte((unsigned char)bytes[i], out);
    }
    // Mid-sequence? The target polls briefly only then — an unambiguous
    // batch pays zero added latency.
    bool pending() const { return _st != state::normal; }
    // The input paused: a pending bare ESC is the esc key; a partial
    // CSI/SS3 is line noise and drops.
    void flush(std::vector<tui_keyev> &out)
    {
	if ( _st == state::esc )
	    emit(out, tui_key::esc);
	_st = state::normal;
	_params.clear();
    }
};

// ----------------------------------------------------------------- the events
// What the application receives: SEMANTIC units, never raw terminal
// events. The target/model pair owns which keys become navigation
// (consumed here, re-render signalled) and which reach the application.
enum class tui_event_kind : unsigned char
{
    none = 0,
    text,	// a coalesced printable run — one semantic insertion
    key,	// a non-printable key for the application to interpret
    choose,	// enter on the focused choice's selected option
    focus,	// focus or menu selection moved: recompose and repaint
    resize,	// the surface changed size: recompose and repaint
    action	// a bound key sequence completed (empty name = unbound miss)
};

struct tui_event
{
    tui_event_kind kind;
    std::string	   text;	// text: the run
    tui_key	   key;		// key: which one (ctrl -> `ch`)
    char	   ch;
    size_t	   option;	// choose: 0-based option index
    name_id	   action;	// choose: the option's first action; 0 = none
    std::string	   action_name;	// action: the bound name ("" = unbound)
    std::string	   seq;		// action: the canonical sequence spelling

    tui_event() : kind(tui_event_kind::none), key(tui_key::none), ch(0),
		  option(0), action(0) {}
};

// ------------------------------------------------------------------ the model
// One instance per TUI session. Contract: compose() before apply_keys()
// (events are interpreted against the focusables the last compose
// discovered), recompose after any focus/resize/text event. Focusable
// identity is discovery order — stable while the application composes
// the same tree shape, which is the Phase-1 contract.
class tui_model
{
public:
    struct focusable
    {
	enum class kind : unsigned char { choice, edit };
	kind k;
	size_t option_count;			// choice: how many options
	std::vector<name_id> option_actions;	// choice: first action each
	focusable() : k(kind::choice), option_count(0) {}
    };

private:
    tui_grid _grid;
    std::vector<focusable> _focusables;
    size_t _focus;
    std::map<size_t, size_t> _selection;	// per choice slot
    std::map<size_t, size_t> _scroll;		// per edit slot: top line
    std::map<size_t, size_t> _hshift;		// per edit slot: left shift
    tui_bindings _bindings;			// the installed profile
    std::string _pending;			// chord so far (canonical)

    // One composed output line: text plus attribute spans.
    struct span { size_t col, len; tui_attr attr; };
    struct line_out
    {
	std::string text;
	std::vector<span> spans;
	line_out() {}
	explicit line_out(const std::string &t) : text(t) {}
    };
    // A flexible edit region parked between fixed lines.
    // A document byte-range with a render style (AST-2 highlight spans):
    // parsed from the edit node's hints["spans"] rows { s, e, c } —
    // byte offsets + a colour NAME (tui_attr_of converts at the
    // boundary; a malformed row is skipped — spans are presentation).
    struct doc_span
    {
	long start, end;
	tui_attr attr;
	doc_span() : start(0), end(0), attr(tui_attr::normal()) {}
    };
    struct edit_slot
    {
	size_t line_index;	// position in the fixed-line stream
	size_t slot;		// focusable index
	std::string text;	// the bound document text
	long caret;		// byte offsets from the node's hints
	long sel_start, sel_end;
	std::vector<doc_span> spans;	// highlight spans (may be empty)
	edit_slot() : line_index(0), slot(0), caret(0),
		      sel_start(-1), sel_end(-1) {}
    };

    static long hint_of(const madc::value &hints, const char *key, long dflt)
    {
	if ( !hints.is_object() )
	    return dflt;
	const std::map<std::string, madc::value> &o = hints.as_object();
	std::map<std::string, madc::value>::const_iterator it = o.find(key);
	if ( it == o.end() || !it->second.is_integer() )
	    return dflt;
	return (long)it->second.as_integer();
    }

    void walk(const roles &r, const uinode &n, size_t cols,
	      std::vector<line_out> &lines, std::vector<edit_slot> &edits)
    {
	if ( n.role == r.heading )
	{
	    // Full-width reverse bar: label left, content right.
	    std::string left = " " + prose::text_of(n.label);
	    std::string right = prose::text_of(n.content);
	    line_out l(left);
	    if ( !right.empty() && left.size() + right.size() + 2 <= cols )
		l.text += std::string(cols - left.size() - right.size() - 1,
				      ' ') + right;
	    span s; s.col = 0; s.len = cols; s.attr = tui_attr::reverse();
	    l.spans.push_back(s);
	    lines.push_back(l);
	}
	else if ( n.role == r.status )
	{
	    line_out l(" " + node_text(n));
	    span s; s.col = 0; s.len = cols; s.attr = tui_attr::reverse();
	    l.spans.push_back(s);
	    lines.push_back(l);
	}
	else if ( n.role == r.content )
	{
	    std::string text = node_text(n);
	    if ( !text.empty() )
	    {
		std::string wrapped = wrap_text(text, cols);
		size_t start = 0;
		for ( size_t i = 0; i <= wrapped.size(); ++i )
		    if ( i == wrapped.size() || wrapped[i] == '\n' )
		    {
			lines.push_back(line_out(wrapped.substr(start,
								i - start)));
			start = i + 1;
		    }
	    }
	}
	else if ( n.role == r.item )
	{
	    lines.push_back(line_out("  " + node_text(n)));
	}
	else if ( n.role == r.action )
	{
	    lines.push_back(line_out("[" + prose::text_of(n.label) + "]"));
	}
	else if ( n.role == r.separator )
	{
	    lines.push_back(line_out(std::string()));
	}
	else if ( n.role == r.choice )
	{
	    // The menu bar: the SAME options line mode numbers, navigable
	    // here — the selected option renders reverse (criterion 4).
	    size_t slot = _focusables.size();
	    focusable f;
	    f.k = focusable::kind::choice;
	    f.option_count = n.children.size();
	    for ( size_t i = 0; i < n.children.size(); ++i )
		f.option_actions.push_back(n.children[i].actions.empty()
					   ? (name_id)0
					   : n.children[i].actions[0]);
	    _focusables.push_back(f);
	    size_t sel = selection_of(slot);
	    line_out l;
	    if ( !n.label.is_null() )
		l.text = prose::text_of(n.label) + " ";
	    for ( size_t i = 0; i < n.children.size(); ++i )
	    {
		std::string opt = " " + node_text(n.children[i]) + " ";
		if ( i == sel )
		{
		    span s;
		    s.col = l.text.size();
		    s.len = opt.size();
		    s.attr = tui_attr::reverse();
		    l.spans.push_back(s);
		}
		l.text += opt;
		if ( i + 1 < n.children.size() )
		    l.text += " ";
	    }
	    lines.push_back(l);
	    return;	// options consumed — no generic child recursion
	}
	else if ( n.role == r.edit )
	{
	    edit_slot e;
	    e.line_index = lines.size();
	    e.slot = _focusables.size();
	    e.text = prose::text_of(n.content);
	    e.caret = hint_of(n.hints, "caret", 0);
	    e.sel_start = hint_of(n.hints, "sel_start", -1);
	    e.sel_end = hint_of(n.hints, "sel_end", -1);
	    if ( n.hints.is_object() )
	    {
		const std::map<std::string, madc::value> &ho = n.hints.as_object();
		std::map<std::string, madc::value>::const_iterator hi =
		    ho.find("spans");
		if ( hi != ho.end() && hi->second.is_array() )
		    for ( const madc::value &row : hi->second.as_array() )
		    {
			if ( !row.is_object() )
			    continue;
			doc_span ds;
			ds.start = hint_of(row, "s", -1);
			ds.end = hint_of(row, "e", -1);
			const std::map<std::string, madc::value> &ro =
			    row.as_object();
			std::map<std::string, madc::value>::const_iterator ci =
			    ro.find("c");
			if ( ds.start < 0 || ds.end <= ds.start
			  || ci == ro.end() || !ci->second.is_string()
			  || !tui_attr_of(ci->second.as_string(), ds.attr) )
			    continue;
			e.spans.push_back(ds);
		    }
	    }
	    edits.push_back(e);
	    focusable f;
	    f.k = focusable::kind::edit;
	    _focusables.push_back(f);
	}
	else if ( n.role == r.list && !n.label.is_null() )
	{
	    lines.push_back(line_out(prose::text_of(n.label) + ":"));
	}
	// group / list / unknown: structure only — children carry it.

	for ( size_t i = 0; i < n.children.size(); ++i )
	    walk(r, n.children[i], cols, lines, edits);
    }

    void paint_line(size_t row, const line_out &l)
    {
	_grid.put(row, 0, l.text);
	for ( size_t i = 0; i < l.spans.size(); ++i )
	    _grid.fill_attr(row, l.spans[i].col, l.spans[i].len,
			    l.spans[i].attr);
    }

    // THE byte->display-column expansion for one document line (tabs move
    // to the next 8-column stop, JOE's default). Returns the display form
    // (what the grid shows); dcol[i] = the display column of byte i, with
    // the end sentinel dcol[size()] = the display width — the ONE map the
    // caret, the horizontal shift, the selection, and the highlight spans
    // all convert through. A control byte other than tab stays one column
    // wide (the grid's put() renders it '?').
    enum { tab_stop = 8 };
    static std::string expand_line(const std::string &line,
				   std::vector<size_t> &dcol)
    {
	std::string disp;
	dcol.assign(line.size() + 1, 0);
	for ( size_t i = 0; i < line.size(); ++i )
	{
	    dcol[i] = disp.size();
	    if ( line[i] == '\t' )
	    {
		disp += ' ';
		while ( disp.size() % tab_stop )
		    disp += ' ';
	    }
	    else
		disp += line[i];
	}
	dcol[line.size()] = disp.size();
	return disp;
    }

    // THE byte-range-to-visible-row overlap rule (selection and highlight
    // spans both paint through it): the [s0, e0) document range's overlap
    // with the line [begin..end] shown at `row`, converted to display
    // columns through the line's expansion map, honoring the horizontal
    // shift and the column clip.
    void fill_range_overlap(size_t row, size_t begin, size_t end,
			    const std::vector<size_t> &dcol,
			    size_t shift, size_t cols,
			    long s0, long e0, tui_attr attr)
    {
	if ( s0 < 0 || e0 <= s0 )
	    return;
	size_t s = (size_t)s0 < begin ? begin : (size_t)s0;
	size_t t = (size_t)e0 > end ? end : (size_t)e0;
	if ( s >= t )
	    return;
	size_t ds = dcol[s - begin];
	size_t dt = dcol[t - begin];
	if ( ds < dt && ds < shift + cols && dt > shift )
	{
	    size_t c0 = ds < shift ? 0 : ds - shift;
	    size_t c1 = dt - shift;
	    _grid.fill_attr(row, c0, c1 - c0, attr);
	}
    }

    // Emit one edit region: a window of the document, scrolled to keep
    // the caret visible, selection byte-range highlighted, the grid
    // cursor on the caret when this edit holds focus.
    void paint_edit(const edit_slot &e, size_t top_row, size_t height,
		    size_t cols)
    {
	// Line starts (byte offsets); the end sentinel makes every offset
	// belong to exactly one line, the caret-at-EOF position included.
	std::vector<size_t> starts;
	starts.push_back(0);
	for ( size_t i = 0; i < e.text.size(); ++i )
	    if ( e.text[i] == '\n' )
		starts.push_back(i + 1);
	size_t caret = e.caret < 0 ? 0
		     : ((size_t)e.caret > e.text.size() ? e.text.size()
							: (size_t)e.caret);
	size_t caret_line = 0;
	while ( caret_line + 1 < starts.size() && starts[caret_line + 1] <= caret )
	    ++caret_line;

	// The caret's DISPLAY column (tab-aware) — the shift and the grid
	// cursor live in display columns; byte offsets convert through the
	// caret line's expansion map.
	size_t caret_begin = starts[caret_line];
	size_t caret_end = caret_line + 1 < starts.size()
			 ? starts[caret_line + 1] - 1 : e.text.size();
	std::vector<size_t> caret_dcol;
	expand_line(e.text.substr(caret_begin, caret_end - caret_begin),
		    caret_dcol);
	size_t caret_col = caret_dcol[caret - caret_begin];

	size_t &top = _scroll[e.slot];
	if ( caret_line < top )
	    top = caret_line;
	if ( caret_line >= top + height )
	    top = caret_line - height + 1;
	if ( top >= starts.size() )
	    top = starts.size() ? starts.size() - 1 : 0;
	size_t &shift = _hshift[e.slot];
	shift = caret_col < cols ? 0 : caret_col - cols + 1;

	for ( size_t k = 0; k < height; ++k )
	{
	    size_t li = top + k;
	    if ( li >= starts.size() )
		break;
	    size_t begin = starts[li];
	    size_t end = li + 1 < starts.size() ? starts[li + 1] - 1
						: e.text.size();
	    std::vector<size_t> dcol;
	    std::string disp = expand_line(e.text.substr(begin, end - begin),
					   dcol);
	    if ( shift < disp.size() )
		_grid.put(top_row + k, 0, disp.substr(shift, cols));
	    // Highlight spans first, the selection LAST (it wins where
	    // they overlap) — both are the one range-overlap rule below.
	    for ( size_t si = 0; si < e.spans.size(); ++si )
		fill_range_overlap(top_row + k, begin, end, dcol, shift, cols,
				   e.spans[si].start, e.spans[si].end,
				   e.spans[si].attr);
	    if ( e.sel_start >= 0 && e.sel_end > e.sel_start )
		fill_range_overlap(top_row + k, begin, end, dcol, shift, cols,
				   e.sel_start, e.sel_end,
				   tui_attr::reverse());
	    if ( li == caret_line && e.slot == _focus )
	    {
		_grid.cursor_row = top_row + k;
		_grid.cursor_col = caret_col - shift;
		_grid.cursor_visible = true;
	    }
	}
    }

public:
    tui_model() : _focus(0) {}

    const tui_grid &grid() const { return _grid; }
    const std::vector<focusable> &focusables() const { return _focusables; }
    size_t focus_slot() const { return _focus; }
    size_t selection_of(size_t slot) const
    {
	std::map<size_t, size_t>::const_iterator it = _selection.find(slot);
	size_t sel = it == _selection.end() ? 0 : it->second;
	if ( slot < _focusables.size()
	  && _focusables[slot].k == focusable::kind::choice
	  && _focusables[slot].option_count > 0
	  && sel >= _focusables[slot].option_count )
	    sel = _focusables[slot].option_count - 1;
	return sel;
    }

    // Compose the tree onto a rows×cols grid. The FIRST edit node is the
    // flexible region (it absorbs the rows fixed content leaves free);
    // later edit nodes get one row each. The tree arrives already
    // access-filtered — composition makes no gate decision.
    const tui_grid &compose(const roles &r, const uinode &tree,
			    size_t rows, size_t cols)
    {
	_grid.resize(rows, cols);
	_focusables.clear();
	std::vector<line_out> lines;
	std::vector<edit_slot> edits;
	walk(r, tree, cols, lines, edits);
	if ( _focus >= _focusables.size() )
	    _focus = 0;

	size_t fixed = lines.size();
	size_t flexible = 0;
	if ( !edits.empty() )
	{
	    size_t others = edits.size() - 1;	// one row each
	    flexible = rows > fixed + others ? rows - fixed - others : 1;
	}
	size_t row = 0, li = 0, ei = 0;
	while ( row < rows && (li < lines.size() || ei < edits.size()) )
	{
	    if ( ei < edits.size() && edits[ei].line_index == li )
	    {
		size_t h = ei == 0 ? flexible : 1;
		if ( h > rows - row )
		    h = rows - row;
		paint_edit(edits[ei], row, h, cols);
		row += h;
		++ei;
		continue;
	    }
	    if ( li < lines.size() )
	    {
		paint_line(row, lines[li]);
		++row;
		++li;
	    }
	}
	return _grid;
    }

    // Install a finalized bindings table (a profile swap is a new table);
    // any chord in flight is abandoned with its profile.
    void set_bindings(const tui_bindings &b)
    {
	_bindings = b;
	_pending.clear();
    }
    const std::string &pending_chord() const { return _pending; }

    // Keys -> semantic events against the last compose's focusables:
    // bound sequences resolve FIRST (a pending chord consumes every key
    // until it completes, misses, or esc cancels it — resize alone passes
    // through); then printable runs coalesce into ONE text event (§7.5);
    // tab cycles focus; arrows navigate a focused choice (selection is
    // presentation state — a focus event says "repaint"); enter on a
    // focused choice chooses; everything else reaches the application as
    // a key event. With no table installed, behavior is byte-identical
    // to the pre-bindings adapter.
    std::vector<tui_event> apply_keys(const std::vector<tui_keyev> &keys)
    {
	std::vector<tui_event> out;
	std::string run;
	for ( size_t i = 0; i < keys.size(); ++i )
	{
	    const tui_keyev &k = keys[i];
	    if ( !_pending.empty() )
	    {
		if ( k.kind == tui_key::resize )
		{
		    tui_event e;
		    e.kind = tui_event_kind::resize;
		    out.push_back(e);
		    continue;
		}
		if ( k.kind == tui_key::esc )
		{
		    // Cancelling a chord is a visible state change — a status
		    // line echoing the prefix (%k) must repaint.
		    _pending.clear();
		    tui_event ec;
		    ec.kind = tui_event_kind::focus;
		    out.push_back(ec);
		    continue;
		}
		std::string candidate = _pending + " "
				      + tui_bindings::cont_spelling(k);
		if ( _bindings.prefix(candidate) )
		{
		    _pending = candidate;
		    tui_event ep;
		    ep.kind = tui_event_kind::focus;
		    out.push_back(ep);
		    continue;
		}
		tui_event e;
		e.kind = tui_event_kind::action;
		e.action_name = _bindings.action_of(candidate);
		e.seq = candidate;
		out.push_back(e);
		_pending.clear();
		continue;
	    }
	    if ( k.kind == tui_key::ch )
	    {
		run += k.ch;
		continue;
	    }
	    if ( !run.empty() )
	    {
		tui_event e;
		e.kind = tui_event_kind::text;
		e.text = run;
		out.push_back(e);
		run.clear();
	    }
	    if ( !_bindings.empty() && k.kind != tui_key::resize )
	    {
		std::string head = tui_bindings::seq_spelling(k);
		if ( _bindings.bound(head) )
		{
		    tui_event e;
		    e.kind = tui_event_kind::action;
		    e.action_name = _bindings.action_of(head);
		    e.seq = head;
		    out.push_back(e);
		    continue;
		}
		if ( _bindings.prefix(head) )
		{
		    // A chord STARTED (and below, extended or cancelled): the
		    // pending prefix is visible state — a status line echoing
		    // it (JOE's %k) needs a repaint event to show it live.
		    _pending = head;
		    tui_event eh;
		    eh.kind = tui_event_kind::focus;
		    out.push_back(eh);
		    continue;
		}
	    }
	    const bool on_choice = _focus < _focusables.size()
		&& _focusables[_focus].k == focusable::kind::choice
		&& _focusables[_focus].option_count > 0;
	    if ( k.kind == tui_key::resize )
	    {
		tui_event e;
		e.kind = tui_event_kind::resize;
		out.push_back(e);
	    }
	    else if ( k.kind == tui_key::tab && _focusables.size() > 1 )
	    {
		_focus = (_focus + 1) % _focusables.size();
		tui_event e;
		e.kind = tui_event_kind::focus;
		out.push_back(e);
	    }
	    else if ( on_choice && (k.kind == tui_key::left
				 || k.kind == tui_key::up
				 || k.kind == tui_key::right
				 || k.kind == tui_key::down) )
	    {
		size_t n = _focusables[_focus].option_count;
		size_t sel = selection_of(_focus);
		if ( k.kind == tui_key::left || k.kind == tui_key::up )
		    sel = (sel + n - 1) % n;
		else
		    sel = (sel + 1) % n;
		_selection[_focus] = sel;
		tui_event e;
		e.kind = tui_event_kind::focus;
		out.push_back(e);
	    }
	    else if ( on_choice && k.kind == tui_key::enter )
	    {
		size_t sel = selection_of(_focus);
		tui_event e;
		e.kind = tui_event_kind::choose;
		e.option = sel;
		e.action = _focusables[_focus].option_actions[sel];
		out.push_back(e);
	    }
	    else
	    {
		tui_event e;
		e.kind = tui_event_kind::key;
		e.key = k.kind;
		e.ch = k.ch;
		out.push_back(e);
	    }
	}
	if ( !run.empty() )
	{
	    tui_event e;
	    e.kind = tui_event_kind::text;
	    e.text = run;
	    out.push_back(e);
	}
	return out;
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_TUI_MODEL_H
