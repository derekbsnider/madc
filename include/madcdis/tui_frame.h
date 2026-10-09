#ifndef __MADCDIS_TUI_FRAME_H
#define __MADCDIS_TUI_FRAME_H 1

// madcdis/tui_frame.h — the TUI's FRAME layer (facelift S2): dividers,
// junctions and (S6) dialog borders as LINES, one glyph per cell decided by
// the cell's ARMS. A line records which way each of its cells reaches (up,
// down, left, right); where two lines meet the arms add, so a panel's top
// divider ending on a sidebar's divider is `┤`, a split's divider standing on
// it `┴`, and nobody chooses a junction glyph by hand.
//
// The model always paints box drawing. A terminal whose locale cannot show it
// spells each glyph in ASCII at emission (ui_box_ascii, read by the VT100
// target beside its colour depth) — one degradation point, as for colour.
//
// Dependency-free beside the grid. THREAD-SAFETY CONTRACT: plain values,
// the C++ standard-library convention (a frame belongs to its model).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "madcdis/tui_grid.h"	// tui_grid — the frame paints into it

namespace madc {
namespace hub {

// Which glyphs a terminal shows (detected once when the terminal target
// opens): Unicode box drawing in a UTF-8 locale, else ASCII.
enum class ui_glyph_set : unsigned char { unicode = 0, ascii };

struct tui_frame
{
    enum : unsigned char { UP = 1, DOWN = 2, LEFT = 4, RIGHT = 8 };

    size_t rows, cols;
    std::vector<unsigned char> arms;	// rows * cols; 0 = no line here

    tui_frame() : rows(0), cols(0) {}
    void reset(size_t r, size_t c)
    {
	rows = r;
	cols = c;
	arms.assign(r * c, 0);
    }
    bool empty_at(size_t r, size_t c) const
	{ return r >= rows || c >= cols || arms[r * cols + c] == 0; }

    // A horizontal line over columns [c0, c1] of `row`: its inner cells reach
    // both ways, its ends only inward (a one-cell line reaches both ways).
    void hline(size_t row, size_t c0, size_t c1)
    {
	if ( row >= rows || c0 > c1 || c0 >= cols )
	    return;
	if ( c1 >= cols )
	    c1 = cols - 1;
	for ( size_t c = c0; c <= c1; ++c )
	{
	    unsigned char a = (c > c0 ? LEFT : 0) | (c < c1 ? RIGHT : 0);
	    arms[row * cols + c] |= a ? a : (LEFT | RIGHT);
	}
    }
    // A vertical line over rows [r0, r1] of `col`, by the same rule.
    void vline(size_t col, size_t r0, size_t r1)
    {
	if ( col >= cols || r0 > r1 || r0 >= rows )
	    return;
	if ( r1 >= rows )
	    r1 = rows - 1;
	for ( size_t r = r0; r <= r1; ++r )
	{
	    unsigned char a = (r > r0 ? UP : 0) | (r < r1 ? DOWN : 0);
	    arms[r * cols + col] |= a ? a : (UP | DOWN);
	}
    }

    // The box-drawing glyph (UTF-8) for a cell's arms. One arm draws as its
    // whole line (a divider's end is still a divider).
    static const char *glyph_of(unsigned char a)
    {
	bool h = (a & (LEFT | RIGHT)) != 0, v = (a & (UP | DOWN)) != 0;
	if ( !v )
	    return "\xe2\x94\x80";				// ─
	if ( !h )
	    return "\xe2\x94\x82";				// │
	switch ( a )
	{
	    case DOWN | RIGHT:		return "\xe2\x94\x8c";	// ┌
	    case DOWN | LEFT:		return "\xe2\x94\x90";	// ┐
	    case UP | RIGHT:		return "\xe2\x94\x94";	// └
	    case UP | LEFT:		return "\xe2\x94\x98";	// ┘
	    case UP | DOWN | RIGHT:	return "\xe2\x94\x9c";	// ├
	    case UP | DOWN | LEFT:	return "\xe2\x94\xa4";	// ┤
	    case DOWN | LEFT | RIGHT:	return "\xe2\x94\xac";	// ┬
	    case UP | LEFT | RIGHT:	return "\xe2\x94\xb4";	// ┴
	    default:			return "\xe2\x94\xbc";	// ┼
	}
    }

    // Every line cell into the grid, in `attr`.
    void paint(tui_grid &g, ui_style attr) const
    {
	for ( size_t r = 0; r < rows && r < g.rows; ++r )
	    for ( size_t c = 0; c < cols && c < g.cols; ++c )
		if ( unsigned char a = arms[r * cols + c] )
		    g.put(r, c, glyph_of(a), attr);
    }
};

// A box-drawing glyph (U+2500..U+257F, packed as tui_cell::ch packs it:
// UTF-8 bytes first-byte-lowest) in ASCII — `-` for a horizontal line, `|`
// for a vertical one, `+` for a corner or junction; 0 = not box drawing.
inline char ui_box_ascii(uint32_t packed)
{
    unsigned b0 = packed & 0xff, b1 = (packed >> 8) & 0xff, b2 = (packed >> 16) & 0xff;
    if ( b0 != 0xe2 || (b1 != 0x94 && b1 != 0x95) || (packed >> 24) != 0 )
	return 0;
    uint32_t cp = ((b0 & 0x0fu) << 12) | ((b1 & 0x3fu) << 6) | (b2 & 0x3fu);
    switch ( cp )
    {
	case 0x2500: case 0x2501: case 0x2504: case 0x2505: case 0x2508:
	case 0x2509: case 0x254c: case 0x254d: case 0x2550:
	    return '-';
	case 0x2502: case 0x2503: case 0x2506: case 0x2507: case 0x250a:
	case 0x250b: case 0x254e: case 0x254f: case 0x2551:
	    return '|';
	default:
	    return '+';
    }
}

} // namespace hub
} // namespace madc

#endif // __MADCDIS_TUI_FRAME_H
