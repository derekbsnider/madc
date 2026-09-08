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
//   - focus and selection: choice options are NAVIGABLE (the same tree
//     line mode numbers — design success criterion 4); focus cycles across
//     choice/edit nodes — compose DISCOVERS the focusables here, the focus
//     slot, the per-choice selection and the tab/arrow/enter rules are the
//     shared owner's (madcdis/ui_focus.h focus_state), consumed by this
//     model and the DOM model alike;
//   - the input adapter: raw terminal bytes → keys (tui_keyparse: CSI/SS3
//     escape parsing with an explicit flush for the bare-ESC pause) and
//     keys → SEMANTIC events, coalescing printable runs into one text
//     event (design §7.5 — five key events never become five domain
//     transactions); the key VOCABULARY, its spelling, the bindings
//     table and the chord resolver are the shared key owner in
//     madcdis/keys.h — this model consumes it (key_resolver::step);
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

#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "madcdis/uinode.h"
#include "madcdis/render_text.h"	// wrap_text — the one wrap owner
#include "madcdis/keys.h"		// tui_key, spelling, tui_bindings, key_resolver
#include "madcdis/ui_events.h"	// tui_event_kind, tui_event
#include "madcdis/ui_focus.h"	// focusable, focus_state — the focus/navigation owner
#include "madcdis/ui_input.h"	// ui_apply_keys — the one keys → events adapter

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
    // One past the rightmost cell EL cannot erase — erase fills with the
    // DEFAULT attributes, so only a normal-attr-space tail qualifies (an
    // inverse status fill does not). A target paints [0..end) and clears
    // the tail with one EL instead of emitting the spaces.
    size_t row_paint_end(size_t r) const
    {
	if ( r >= rows )
	    return 0;
	size_t end = cols;
	while ( end > 0 )
	{
	    const tui_cell &cell = at(r, end - 1);
	    if ( cell.ch != ' ' || !cell.attr.is_normal() )
		break;
	    --end;
	}
	return end;
    }
};

// Whole-row equality between two same-width grids.
inline bool tui_rows_equal(const tui_grid &a, size_t ra,
			   const tui_grid &b, size_t rb)
{
    for ( size_t c = 0; c < a.cols; ++c )
	if ( a.at(ra, c) != b.at(rb, c) )
	    return false;
    return true;
}

// A repaint span: cells [c0..c1] of one row. Row-level diffing repaints
// 80 columns when two digits change; the span is the cell-level truth
// (JOE's update granularity) and the unit every target emits.
struct tui_row_span
{
    size_t row, c0, c1;
    tui_row_span(size_t r, size_t a, size_t b) : row(r), c0(a), c1(b) {}
};

// The differing spans between two grids: per changed row, the first and
// last differing cell. A dimension change is a full repaint of every row.
inline std::vector<tui_row_span> tui_diff_spans(const tui_grid &prev,
						const tui_grid &next)
{
    std::vector<tui_row_span> out;
    if ( prev.rows != next.rows || prev.cols != next.cols )
    {
	for ( size_t r = 0; r < next.rows; ++r )
	    out.push_back(tui_row_span(r, 0, next.cols ? next.cols - 1 : 0));
	return out;
    }
    for ( size_t r = 0; r < next.rows; ++r )
    {
	size_t c0 = next.cols, c1 = 0;
	for ( size_t c = 0; c < next.cols; ++c )
	    if ( prev.at(r, c) != next.at(r, c) )
	    {
		if ( c0 == next.cols )
		    c0 = c;
		c1 = c;
	    }
	if ( c0 != next.cols )
	    out.push_back(tui_row_span(r, c0, c1));
    }
    return out;
}

// Rows differing between two grids — the span diff's row view (ONE cell
// comparison loop owns both granularities).
inline std::vector<size_t> tui_dirty_rows(const tui_grid &prev,
					  const tui_grid &next)
{
    std::vector<tui_row_span> spans = tui_diff_spans(prev, next);
    std::vector<size_t> out;
    for ( size_t i = 0; i < spans.size(); ++i )
	out.push_back(spans[i].row);
    return out;
}

// FNV-1a over a row's cells (glyph + attribute bytes) — the O(rows*cols)
// prefilter that keeps tui_diff_plan's offset scan at O(rows^2) hash
// compares; equality is always confirmed by tui_rows_equal on a hit.
inline uint64_t tui_row_hash(const tui_grid &g, size_t r)
{
    uint64_t h = 1469598103934665603ULL;	// 64-bit even on LLP64
    for ( size_t c = 0; c < g.cols; ++c )
    {
	const tui_cell &cell = g.at(r, c);
	unsigned char bytes[4] = { (unsigned char)cell.ch, cell.attr.fg,
				   cell.attr.bg, cell.attr.flags };
	for ( int i = 0; i < 4; ++i )
	{
	    h ^= bytes[i];
	    h *= 1099511628211ULL;
	}
    }
    return h;
}

// A repaint PLAN between two grids — differential support, level 2 (the
// scroll-feel half of IDE-9c). Either the plain diff spans (shifted ==
// false) or a vertical scroll: the terminal moves rows `delta` lines
// (up == toward row 0) inside the region [top..bot], then `spans`
// repaint. A VT100-family target emits the shift as DECSTBM + DL/IL
// (JOE's own dl/al); the blanks the terminal inserts at the region's far
// edge carry the default attributes.
//
// The repaint set is computed by SIMULATION: apply the shift to `prev`,
// re-diff against `next`. Whatever the offset scan guessed, painting
// plan.spans after the shift reproduces `next` exactly — detection
// quality only affects how MUCH repaints, never what the screen shows.
// The shift is taken only when its estimated emission cost (span widths
// + per-span addressing + the ~30-byte scroll op) beats the plain diff's.
struct tui_paint_plan
{
    bool		shifted;
    bool		up;	// content moves toward row 0 (DL); else IL
    size_t		top, bot;	// scroll region, inclusive
    size_t		delta;		// lines moved
    std::vector<tui_row_span> spans;	// repaint AFTER the shift
    tui_paint_plan() : shifted(false), up(false), top(0), bot(0), delta(0) {}
};

// Estimated bytes to emit a span set: cells + ~10 addressing/SGR bytes each.
inline size_t tui_span_cost(const std::vector<tui_row_span> &spans)
{
    size_t cost = 0;
    for ( size_t i = 0; i < spans.size(); ++i )
	cost += spans[i].c1 - spans[i].c0 + 1 + 10;
    return cost;
}

inline tui_paint_plan tui_diff_plan(const tui_grid &prev, const tui_grid &next)
{
    tui_paint_plan plan;
    plan.spans = tui_diff_spans(prev, next);
    if ( prev.rows != next.rows || prev.cols != next.cols
      || plan.spans.size() < 4 )
	return plan;
    std::vector<bool> is_dirty(next.rows, false);
    for ( size_t i = 0; i < plan.spans.size(); ++i )
	is_dirty[plan.spans[i].row] = true;
    std::vector<uint64_t> ph(next.rows), nh(next.rows);
    for ( size_t r = 0; r < next.rows; ++r )
    {
	ph[r] = tui_row_hash(prev, r);
	nh[r] = tui_row_hash(next, r);
    }
    // The moved band: the run of rows matching prev at one vertical
    // offset covering the most DIRTY rows (unchanged rows also match at
    // offset 0 and prove nothing — only dirty rows are evidence).
    size_t best_score = 0, best_a = 0, best_b = 0, best_delta = 0;
    bool   best_up = false;
    for ( size_t delta = 1; delta < next.rows; ++delta )
    {
	for ( int dir = 0; dir < 2; ++dir )
	{
	    bool up = dir == 0;
	    size_t r = 0;
	    while ( r < next.rows )
	    {
		size_t from = up ? r + delta : r - delta;
		bool ok = (up ? r + delta < next.rows : r >= delta)
		       && nh[r] == ph[from]
		       && tui_rows_equal(next, r, prev, from);
		if ( !ok )
		{
		    ++r;
		    continue;
		}
		size_t a = r, score = 0;
		while ( ok )
		{
		    score += is_dirty[r] ? 1 : 0;
		    ++r;
		    from = up ? r + delta : r - delta;
		    ok = r < next.rows
		      && (up ? r + delta < next.rows : r >= delta)
		      && nh[r] == ph[from]
		      && tui_rows_equal(next, r, prev, from);
		}
		if ( score > best_score )
		{
		    best_score = score;
		    best_a = a;
		    best_b = r - 1;
		    best_delta = delta;
		    best_up = up;
		}
	    }
	}
    }
    if ( best_score == 0 )
	return plan;
    // The region spans the band plus the rows the shift consumes: up (DL
    // at top) region = [a .. b+delta]; down (IL at top) = [a-delta .. b].
    size_t T = best_up ? best_a : best_a - best_delta;
    size_t B = best_up ? best_b + best_delta : best_b;
    // Simulate the shift on prev, re-diff: the exact repaint set.
    tui_grid shifted = prev;
    for ( size_t r = T; r <= B; ++r )
    {
	bool   from_ok = best_up ? r + best_delta <= B : r >= T + best_delta;
	size_t from = best_up ? r + best_delta
			      : (from_ok ? r - best_delta : 0);
	for ( size_t c = 0; c < next.cols; ++c )
	    shifted.at(r, c) = from_ok ? prev.at(from, c) : tui_cell();
    }
    std::vector<tui_row_span> after = tui_diff_spans(shifted, next);
    if ( tui_span_cost(after) + 30 >= tui_span_cost(plan.spans) )
	return plan;		// the shift would not pay for itself
    plan.shifted = true;
    plan.up = best_up;
    plan.top = T;
    plan.bot = B;
    plan.delta = best_delta;
    plan.spans = after;
    return plan;
}

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

// ------------------------------------------------------------------ the model
// One instance per TUI session. Contract: compose() before apply_keys()
// (events are interpreted against the focusables the last compose
// discovered), recompose after any focus/resize/text event. Focusable
// identity is discovery order — stable while the application composes
// the same tree shape, which is the Phase-1 contract.
class tui_model
{
public:
    // The focusable vocabulary is the shared focus owner's (madcdis/
    // ui_focus.h); the model's spelling stays for its consumers.
    typedef madc::hub::focusable focusable;

private:
    tui_grid _grid;
    focus_state _focus_st;			// focus slot + per-choice selection + navigation
    std::map<size_t, size_t> _scroll;		// per edit slot: top line
    std::map<size_t, size_t> _hshift;		// per edit slot: left shift
    key_resolver _keys;			// the ONE chord/key owner (madcdis/keys.h)

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
	long tabw;		// display tab width (hints["tabwidth"], the
				// ^T option); clamped 1..16 at parse
	long rows;		// fixed height (hints["rows"], IDE-9e
				// windows — the composer owns the split
				// math and carries it as DATA); 0 = the
				// legacy rule (first unhinted flexible,
				// other unhinted one row — prompts)
	std::vector<doc_span> spans;	// highlight spans (may be empty)
	edit_slot() : line_index(0), slot(0), caret(0),
		      sel_start(-1), sel_end(-1), tabw(tab_stop),
		      rows(0) {}
    };

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
	    // Two PRESENTATIONS of one focusable (hints, data-driven):
	    //   list:1  — the popup-list shape (IDE-10a palettes): label
	    //             row, then ONE OPTION PER ROW, selected reversed.
	    //             Same navigation/choose semantics as the bar.
	    //   focus:1 — autofocus: arrows/enter land on this choice
	    //             without a tab cycle (a palette is modal while
	    //             up; when its node vanishes, focus resets).
	    size_t slot = _focus_st.count();
	    focusable f;
	    f.k = focusable::kind::choice;
	    f.option_count = n.children.size();
	    for ( size_t i = 0; i < n.children.size(); ++i )
		f.option_actions.push_back(n.children[i].actions.empty()
					   ? (name_id)0
					   : n.children[i].actions[0]);
	    _focus_st.add(f);
	    if ( hint_of(n.hints, "focus", 0) )
		_focus_st.set_focus(slot);
	    size_t sel = selection_of(slot);
	    if ( hint_of(n.hints, "list", 0) )
	    {
		if ( !n.label.is_null() )
		    lines.push_back(line_out(prose::text_of(n.label)));
		for ( size_t i = 0; i < n.children.size(); ++i )
		{
		    line_out l;
		    l.text = "  " + node_text(n.children[i]);
		    if ( i == sel )
		    {
			span s;
			s.col = 0;
			s.len = l.text.size();
			s.attr = tui_attr::reverse();
			l.spans.push_back(s);
		    }
		    lines.push_back(l);
		}
		return;	// options consumed — no generic child recursion
	    }
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
	    e.slot = _focus_st.count();
	    e.text = prose::text_of(n.content);
	    e.caret = hint_of(n.hints, "caret", 0);
	    e.sel_start = hint_of(n.hints, "sel_start", -1);
	    e.sel_end = hint_of(n.hints, "sel_end", -1);
	    e.tabw = hint_of(n.hints, "tabwidth", tab_stop);
	    if ( e.tabw < 1 )
		e.tabw = 1;
	    if ( e.tabw > 16 )
		e.tabw = 16;
	    e.rows = hint_of(n.hints, "rows", 0);
	    if ( e.rows < 0 )
		e.rows = 0;
	    // The same autofocus hint the choice arm honors (IDE-9e: the
	    // active window's edit node carries it).
	    if ( hint_of(n.hints, "focus", 0) )
		_focus_st.set_focus(e.slot);
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
	    _focus_st.add(f);
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
				   std::vector<size_t> &dcol,
				   size_t tabw = tab_stop)
    {
	std::string disp;
	if ( tabw < 1 )
	    tabw = tab_stop;
	dcol.assign(line.size() + 1, 0);
	for ( size_t i = 0; i < line.size(); ++i )
	{
	    dcol[i] = disp.size();
	    if ( line[i] == '\t' )
	    {
		disp += ' ';
		while ( disp.size() % tabw )
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
		    caret_dcol, (size_t)e.tabw);
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
					   dcol, (size_t)e.tabw);
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
	    if ( li == caret_line && e.slot == _focus_st.focus() )
	    {
		_grid.cursor_row = top_row + k;
		_grid.cursor_col = caret_col - shift;
		_grid.cursor_visible = true;
	    }
	}
    }

public:
    tui_model() {}

    const tui_grid &grid() const { return _grid; }
    // Focus and selection are the shared owner's (madcdis/ui_focus.h);
    // the model's spellings forward.
    const std::vector<focusable> &focusables() const { return _focus_st.focusables(); }
    size_t focus_slot() const { return _focus_st.focus(); }
    size_t selection_of(size_t slot) const { return _focus_st.selection_of(slot); }

    // Compose the tree onto a rows×cols grid. Edit heights are DATA
    // (IDE-9e): a node hinted rows:N is FIXED at N; among the unhinted,
    // the FIRST is the flexible region (it absorbs the rows fixed content
    // and hinted edits leave free) and the rest get one row each — the
    // prompt shape, byte-identical when nothing is hinted. The tree
    // arrives already access-filtered — composition makes no gate
    // decision.
    const tui_grid &compose(const roles &r, const uinode &tree,
			    size_t rows, size_t cols)
    {
	_grid.resize(rows, cols);
	_focus_st.begin_compose();
	std::vector<line_out> lines;
	std::vector<edit_slot> edits;
	walk(r, tree, cols, lines, edits);
	_focus_st.end_compose();

	size_t fixed = lines.size();
	size_t hinted_sum = 0, unhinted = 0;
	for ( size_t i = 0; i < edits.size(); ++i )
	{
	    if ( edits[i].rows > 0 )
		hinted_sum += (size_t)edits[i].rows;
	    else
		++unhinted;
	}
	size_t flexible = 0;
	if ( unhinted )
	{
	    size_t others = unhinted - 1;	// one row each
	    size_t taken = fixed + hinted_sum + others;
	    flexible = rows > taken ? rows - taken : 1;
	}
	size_t row = 0, li = 0, ei = 0;
	bool flex_spent = false;
	while ( row < rows && (li < lines.size() || ei < edits.size()) )
	{
	    if ( ei < edits.size() && edits[ei].line_index == li )
	    {
		size_t h;
		if ( edits[ei].rows > 0 )
		    h = (size_t)edits[ei].rows;
		else if ( !flex_spent )
		{
		    h = flexible;
		    flex_spent = true;
		}
		else
		    h = 1;
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
    // any chord in flight is abandoned with its profile. The table and the
    // chord in flight live in the key owner (madcdis/keys.h).
    void set_bindings(const tui_bindings &b) { _keys.set_bindings(b); }
    const std::string &pending_chord() const { return _keys.pending(); }

    // Keys -> semantic events against the last compose's focusables:
    // bound sequences resolve FIRST (a pending chord consumes every key
    // until it completes, misses, or esc cancels it — resize alone passes
    // through); then printable runs coalesce into ONE text event (§7.5);
    // tab cycles focus; arrows navigate a focused choice (selection is
    // presentation state — a focus event says "repaint"); enter on a
    // focused choice chooses; everything else reaches the application as
    // a key event. With no table installed, behavior is byte-identical
    // to the pre-bindings adapter. The loop itself is the shared adapter
    // ui_apply_keys (madcdis/ui_input.h) — the DOM model runs the same one.
    std::vector<tui_event> apply_keys(const std::vector<tui_keyev> &keys)
    {
	// The one adapter (madcdis/ui_input.h) over this model's two owners.
	return ui_apply_keys(_keys, _focus_st, keys);
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_TUI_MODEL_H
