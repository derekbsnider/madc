#ifndef __MADCDIS_UI_STYLE_H
#define __MADCDIS_UI_STYLE_H 1

// madcdis/ui_style.h — the ONE render style shared by every ui renderer
// (AST-2; owner: VT-102's ANSI colours, JOE parity): what JOE's syntax
// vocabulary can say — the classic attributes plus an 8-colour
// foreground/background, 16 effective foreground colours via
// bold-as-bright (the VT-102/16-colour model; no aixterm 90–97) — and an
// EXACT colour (`#rrggbb`, TUI facelift S1): the theme's value, which the
// window shows as it is and a terminal shows at its colour depth (truecolor,
// the nearest of 256, else the 8-colour index the parser also records).
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
// seat filled by S1: the exact colour rides beside the index.
//
// Dependency-free: <string> and <cstdint> only.
//
// Thread contract: plain values; the C++ standard-library convention.

#include <cstdint>
#include <cstdio>
#include <string>

namespace madc {
namespace hub {

// How many colours a terminal shows (detected once when the terminal target
// opens): the 8/16-colour index, the xterm 256 palette, or 24-bit RGB.
enum class ui_colour_depth : unsigned char { ansi16 = 0, xterm256, truecolor };

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
    // The EXACT colour when the spec named one (`#rrggbb`): RGB_SET | 0xRRGGBB,
    // 0 = none. fg / bg then hold its nearest 8-colour index (ui_rgb_nearest_ansi),
    // so a renderer that knows only the index needs nothing more.
    enum : uint32_t { RGB_SET = 0x01000000u };
    uint32_t fg_rgb;
    uint32_t bg_rgb;
    ui_style() : fg(0), bg(0), flags(0), fg_rgb(0), bg_rgb(0) {}
    bool operator==(const ui_style &o) const
	{ return fg == o.fg && bg == o.bg && flags == o.flags
	      && fg_rgb == o.fg_rgb && bg_rgb == o.bg_rgb; }
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
// `bg_<colour>`, an exact colour `#rrggbb` / `bg_#rrggbb` (the value plus its
// nearest 8-colour index), and `normal` (alone) for the default style. False =
// any unknown word (the WHOLE spec is refused — themes fail loud).
// The nearest of the 8 ANSI colours (the 1..8 index) to an RGB value, by
// HUE: a colour whose saturation is low reads as black or white by its value;
// any other takes the nearest of the six hues (red yellow green cyan blue
// magenta). Plain RGB distance picks badly here — Dark+'s comment green
// #6a9955 lies nearer xterm's yellow than its green — while the hue is what
// a reader sees.
inline unsigned char ui_rgb_nearest_ansi(uint32_t rgb)
{
    int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if ( mx == 0 || (mx - mn) * 100 < mx * 25 )
	return mx < 128 ? 1 : 8;		// black / white
    int d = mx - mn;
    int hue;					// degrees, 0..359
    if ( mx == r )
	hue = (60 * (g - b) / d + 360) % 360;
    else if ( mx == g )
	hue = 60 * (b - r) / d + 120;
    else
	hue = 60 * (r - g) / d + 240;
    // ANSI order: 2 red, 4 yellow, 3 green, 7 cyan, 5 blue, 6 magenta
    static const unsigned char by_sextant[6] = { 2, 4, 3, 7, 5, 6 };
    return by_sextant[((hue + 30) / 60) % 6];
}

// The nearest entry of the xterm 256-colour palette (the 6x6x6 cube or the
// 24-step grey ramp, whichever is closer) — a 256-colour terminal's SGR 38;5.
inline int ui_rgb_nearest_256(uint32_t rgb)
{
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    int c[3] = { (int)((rgb >> 16) & 0xff), (int)((rgb >> 8) & 0xff), (int)(rgb & 0xff) };
    int idx[3];
    for ( int k = 0; k < 3; ++k )
    {
	int best = 0;
	for ( int j = 1; j < 6; ++j )
	{
	    int dj = c[k] - lv[j], db = c[k] - lv[best];
	    if ( dj * dj < db * db )
		best = j;
	}
	idx[k] = best;
    }
    int cube = 16 + 36 * idx[0] + 6 * idx[1] + idx[2];
    int cd = 0;
    for ( int k = 0; k < 3; ++k )
	cd += (c[k] - lv[idx[k]]) * (c[k] - lv[idx[k]]);
    int avg = (c[0] + c[1] + c[2]) / 3;
    int step = avg < 8 ? 0 : avg > 238 ? 23 : (avg - 8) / 10;
    int gv = 8 + 10 * step;
    int gd = 0;
    for ( int k = 0; k < 3; ++k )
	gd += (c[k] - gv) * (c[k] - gv);
    return gd < cd ? 232 + step : cube;
}

// `#rrggbb` -> 0xRRGGBB; false when the word is not exactly that.
inline bool ui_rgb_of(const std::string &w, uint32_t &rgb)
{
    if ( w.size() != 7 || w[0] != '#' )
	return false;
    uint32_t v = 0;
    for ( size_t i = 1; i < 7; ++i )
    {
	char ch = w[i];
	int d = ch >= '0' && ch <= '9' ? ch - '0'
	      : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
	      : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
	if ( d < 0 )
	    return false;
	v = (v << 4) | (uint32_t)d;
    }
    rgb = v;
    return true;
}

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
	uint32_t rgb = 0;
	if ( ui_rgb_of(w, rgb) )
	{
	    a.fg_rgb = ui_style::RGB_SET | rgb;
	    a.fg = ui_rgb_nearest_ansi(rgb);
	    any = true;
	    continue;
	}
	if ( w.compare(0, 3, "bg_") == 0 && ui_rgb_of(w.substr(3), rgb) )
	{
	    a.bg_rgb = ui_style::RGB_SET | rgb;
	    a.bg = ui_rgb_nearest_ansi(rgb);
	    any = true;
	    continue;
	}
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

// The xterm 256-colour palette's entry `n` as 0xRRGGBB: the 16 system
// colours (xterm's defaults), the 6x6x6 cube, the 24-step grey ramp — the
// inverse of ui_rgb_nearest_256 (a program's SGR 38;5;n, facelift S8).
inline uint32_t ui_xterm256_rgb(int n)
{
    static const uint32_t sys[16] = {
	0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
	0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff
    };
    if ( n < 0 || n > 255 )
	return 0;
    if ( n < 16 )
	return sys[n];
    if ( n >= 232 )
    {
	uint32_t g = (uint32_t)(8 + 10 * (n - 232));
	return (g << 16) | (g << 8) | g;
    }
    static const uint32_t lv[6] = { 0, 95, 135, 175, 215, 255 };
    n -= 16;
    return (lv[n / 36] << 16) | (lv[(n / 6) % 6] << 8) | lv[n % 6];
}

// A style as the spec ui_style_of reads back to the SAME style — the
// inverse of the one parser (a terminal program's colours carried as span
// specs, facelift S8): attribute words, then the foreground and background
// (an exact colour as #rrggbb, else the colour's name); "normal" for none.
inline std::string ui_style_spec(const ui_style &s)
{
    static const struct { unsigned char bit; const char *word; } attrs[] = {
	{ ui_style::BOLD, "bold" }, { ui_style::DIM, "dim" },
	{ ui_style::ITALIC, "italic" }, { ui_style::UNDERLINE, "underline" },
	{ ui_style::BLINK, "blink" }, { ui_style::INVERSE, "inverse" }
    };
    std::string out;
    for ( size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); ++i )
	if ( s.flags & attrs[i].bit )
	    out += std::string(out.empty() ? "" : " ") + attrs[i].word;
    char hex[8];
    if ( s.fg_rgb & ui_style::RGB_SET )
    {
	snprintf(hex, sizeof(hex), "#%06x", (unsigned)(s.fg_rgb & 0xffffff));
	out += std::string(out.empty() ? "" : " ") + hex;
    }
    else if ( s.fg >= 1 && s.fg <= 8 )
	out += std::string(out.empty() ? "" : " ") + ui_style_colour_name(s.fg);
    if ( s.bg_rgb & ui_style::RGB_SET )
    {
	snprintf(hex, sizeof(hex), "#%06x", (unsigned)(s.bg_rgb & 0xffffff));
	out += std::string(out.empty() ? "" : " ") + "bg_" + hex;
    }
    else if ( s.bg >= 1 && s.bg <= 8 )
	out += std::string(out.empty() ? "" : " ") + "bg_" + ui_style_colour_name(s.bg);
    return out.empty() ? std::string("normal") : out;
}

} // namespace hub
} // namespace madc

#endif // __MADCDIS_UI_STYLE_H
