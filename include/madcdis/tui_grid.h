#ifndef __MADCDIS_TUI_GRID_H
#define __MADCDIS_TUI_GRID_H 1

// madcdis/tui_grid.h — the level-1 renderer's cell GRID and its repaint
// diff (split out of tui_model.h, TUI facelift S0): tui_cell / tui_grid,
// the dirty-row spans and the paint plan a target emits. Pure data, no
// terminal I/O; tui_model composes onto it, the target paints from it.
//
// THREAD-SAFETY CONTRACT: plain values, the C++ standard-library
// convention (a grid is confined to the frontend that owns it).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "madcdis/ui_style.h"	// ui_style — the one render style
#include "madcdis/text_utf16.h"	// line_layout / line_columns — the one line layout (B87)

namespace madc {
namespace hub {

// ------------------------------------------------------------------ the grid
// The render STYLE (ui_style) and its spec parser (ui_style_of) live in
// madcdis/ui_style.h — the one vocabulary the DOM model renders too; this
// model paints it into cells, the VT100 target spells it as SGR.

// One terminal column. `ch` is the glyph drawn there: one code point's UTF-8
// bytes packed first-byte-lowest (an ASCII glyph is its own byte, so a cell
// compares against 'x' directly). A wide glyph (codepoint_columns() == 2)
// occupies its cell AND the next, which is its `tail`: no glyph of its own,
// never emitted (the terminal advanced over it drawing the glyph).
struct tui_cell
{
    uint32_t ch;
    bool     tail;
    ui_style attr;
    tui_cell() : ch(' '), tail(false), attr(ui_style::normal()) {}
    bool operator==(const tui_cell &o) const
	{ return ch == o.ch && tail == o.tail && attr == o.attr; }
    bool operator!=(const tui_cell &o) const { return !(*this == o); }
    // The glyph's bytes, appended to `out` (nothing for a tail).
    void append_glyph(std::string &out) const
    {
	if ( tail )
	    return;
	for ( uint32_t g = ch; g != 0; g >>= 8 )
	    out += (char)(g & 0xFF);
    }
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
    // printable glyph and the grid's columns ARE the screen's — a control
    // byte in a cell desynchronizes them (a raw tab MOVES the terminal cursor
    // without erasing the skipped columns: stale fragments + doubled glyphs
    // while scrolling, the IDE-9c defect), and so does a character's bytes
    // counted as columns (B87: the caret drifted one column right per extra
    // byte of a UTF-8 character). Tab expansion is the document projection's
    // job (paint_edit lays lines out through madc::line_layout); here every
    // control byte renders as a visible '?', each code point takes
    // codepoint_columns() cells, and a wide glyph that does not fit before
    // the right edge shows as a space.
    void put(size_t r, size_t c, const std::string &text,
	     ui_style attr = ui_style::normal())
    {
	if ( r >= rows )
	    return;
	size_t i = 0;
	while ( i < text.size() && c < cols )
	{
	    uint32_t cp = 0;
	    size_t n = madc::utf8_decode_at(text, i, cp);
	    uint32_t glyph = 0;
	    size_t w = 1;
	    if ( cp < 0x20 || cp == 0x7f )
		glyph = '?';
	    else
	    {
		for ( size_t k = n; k > 0; --k )
		    glyph = (glyph << 8) | (unsigned char)text[i + k - 1];
		w = madc::codepoint_columns(cp);
	    }
	    if ( c + w > cols )
	    {
		glyph = ' ';		// a wide glyph cut by the right edge
		w = 1;
	    }
	    set_glyph(r, c, glyph, w, attr);
	    c += w;
	    i += n;
	}
    }
    // Place one glyph `w` columns wide at (r, c). A glyph overwriting half of
    // a wide one leaves the other half a space, so no tail outlives its lead
    // and no lead loses its tail.
    void set_glyph(size_t r, size_t c, uint32_t glyph, size_t w, ui_style attr)
    {
	if ( at(r, c).tail && c > 0 )
	{
	    at(r, c - 1).ch = ' ';
	    at(r, c - 1).tail = false;
	}
	size_t after = c + w;
	if ( after < cols && at(r, after).tail )
	{
	    at(r, after).ch = ' ';
	    at(r, after).tail = false;
	}
	tui_cell &cell = at(r, c);
	cell.ch = glyph;
	cell.tail = false;
	cell.attr = attr;
	if ( w == 2 )
	{
	    tui_cell &t2 = at(r, c + 1);
	    t2.ch = 0;
	    t2.tail = true;
	    t2.attr = attr;
	}
    }
    void fill_attr(size_t r, size_t c, size_t len, ui_style attr)
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
	    at(r, c).append_glyph(out);
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
	unsigned char bytes[8] = { (unsigned char)(cell.ch & 0xFF),
				   (unsigned char)((cell.ch >> 8) & 0xFF),
				   (unsigned char)((cell.ch >> 16) & 0xFF),
				   (unsigned char)(cell.ch >> 24),
				   (unsigned char)cell.tail, cell.attr.fg,
				   cell.attr.bg, cell.attr.flags };
	for ( int i = 0; i < 8; ++i )
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


} // namespace hub
} // namespace madc

#endif // __MADCDIS_TUI_GRID_H
