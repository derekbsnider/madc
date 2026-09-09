#ifndef __MADCDIS_UI_STYLE_H
#define __MADCDIS_UI_STYLE_H 1

// madcdis/ui_style.h — the ONE render style shared by every ui renderer
// (AST-2; owner: VT-102's ANSI colours, JOE parity): what JOE's syntax
// vocabulary can say — the classic attributes plus an 8-colour
// foreground/background, 16 effective foreground colours via
// bold-as-bright (the VT-102/16-colour model; no aixterm 90–97).
//
// A THEME (app data, profiles/*.theme) maps classification names to style
// SPECS in this vocabulary; ui_style_of below is the one spec parser at the
// value boundary. Each renderer owns only its LAST step from the style:
// the VT100 target (src/ui_term.cpp) style -> SGR, the DOM model
// (madcdis/web_model.h) style -> CSS classes over the page's palette. Moved
// out of tui_model.h (2026-09-08, the colour-unification slice): the web
// renderer used to style the span's semantic CLASS from a palette of its
// own, so the GUI never showed the theme the terminal showed — a copy of a
// vocabulary is where two renderers drift. 256/true-colour is a named
// later seat.
//
// Dependency-free: <string> only.
//
// Thread contract: plain values; the C++ standard-library convention.

#include <string>

namespace madc {
namespace hub {

struct ui_style
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
    ui_style() : fg(0), bg(0), flags(0) {}
    bool operator==(const ui_style &o) const
	{ return fg == o.fg && bg == o.bg && flags == o.flags; }
    bool operator!=(const ui_style &o) const { return !(*this == o); }
    bool is_normal() const { return fg == 0 && bg == 0 && flags == 0; }
    // Pure inverse — the pre-colour renderer's one non-normal style; the
    // VT100 target keeps its historical \x1b[7m spelling for it.
    bool is_reverse() const { return fg == 0 && bg == 0 && flags == INVERSE; }
    static ui_style normal() { return ui_style(); }
    static ui_style reverse()
	{ ui_style a; a.flags = INVERSE; return a; }
};

// The 8 colour NAMES (ANSI order) — the one table the parser reads and a
// renderer spells back (the DOM model's fg-<name> / bg-<name> classes).
// `index` is the 1..8 domain of ui_style::fg / bg; 0 or out of range = "".
inline const char *ui_style_colour_name(unsigned char index)
{
    static const char *const colours[8] = {
	"black", "red", "green", "yellow",
	"blue", "magenta", "cyan", "white"
    };
    return index >= 1 && index <= 8 ? colours[index - 1] : "";
}

// THE style-spec parser (JOE's vocabulary, one table): space-separated
// words — attributes `bold dim italic underline blink inverse` (JOE's
// `reverse` accepted as a synonym), a foreground colour word
// `black red green yellow blue magenta cyan white`, a background
// `bg_<colour>`, and `normal` (alone) for the default style. False =
// any unknown word (the WHOLE spec is refused — themes fail loud).
inline bool ui_style_of(const std::string &spec, ui_style &out)
{
    ui_style a;
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
	if ( w == "bold" )	    { a.flags |= ui_style::BOLD; any = true; continue; }
	if ( w == "dim" )	    { a.flags |= ui_style::DIM; any = true; continue; }
	if ( w == "italic" )	    { a.flags |= ui_style::ITALIC; any = true; continue; }
	if ( w == "underline" )	    { a.flags |= ui_style::UNDERLINE; any = true; continue; }
	if ( w == "blink" )	    { a.flags |= ui_style::BLINK; any = true; continue; }
	if ( w == "inverse" || w == "reverse" )
				    { a.flags |= ui_style::INVERSE; any = true; continue; }
	bool matched = false;
	for ( unsigned char c = 1; c <= 8 && !matched; ++c )
	{
	    const char *name = ui_style_colour_name(c);
	    if ( w == name )
	    {
		a.fg = c;
		matched = true;
	    }
	    else if ( w.compare(0, 3, "bg_") == 0
		   && w.compare(3, std::string::npos, name) == 0 )
	    {
		a.bg = c;
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

} // namespace hub
} // namespace madc

#endif // __MADCDIS_UI_STYLE_H
