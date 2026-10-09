#ifndef __MADCDIS_TERM_SCREEN_H
#define __MADCDIS_TERM_SCREEN_H 1

// madcdis/term_screen.h — a BOUNDED terminal screen for madcide's embedded
// Terminal tab (polish P3b-2): the bytes a program writes to its pty,
// folded into the text a document shows. Not a VT100 emulator — the part of
// one a scrollback pane needs: printable bytes land at the cursor column of
// the current line (overwriting what is there, as a terminal does after a
// carriage return); `\n` completes the line, `\r` returns to column 0,
// `\b` steps back, `\t` advances to the next 8-column stop, BEL and the
// other C0 controls are dropped; escape sequences are parsed and DROPPED —
// except CSI `K` (erase to the end of the line) and CSI `2J` (clear the
// screen), which a progress bar / a `clear` need to look right; OSC
// sequences (window titles) end at BEL or ESC \. SGR sets the PEN (facelift
// S8): attributes, the 8 / 16 / 256 colours and 24-bit colour onto the one
// style (ui_style), and every byte a program writes keeps the pen it was
// written with — spans() hands them out as {start, end, spec} over the
// text, in the theme's spec vocabulary both faces render. Byte-oriented: a
// multi-byte UTF-8 glyph occupies one column per byte, as the grid model's
// pilot does. The scrollback keeps at most `cap` completed lines.
//
// The state is a VALUE: the completed lines' text, the current line, the
// cursor column, the escape parser's state, the pen and the spans — so an
// owner may keep it on a document (the text + a few attributes) between
// feeds, feeding each chunk the pump reads. Dependency-free beside
// madcdis/ui_style.h: <string> <vector>.
//
// Thread contract: a plain value object; confined with the pump that
// feeds it (the C++ standard-library convention).

#include <cstdlib>
#include <string>
#include <vector>

#include "madcdis/ui_style.h"	// ui_style, ui_style_of / ui_style_spec — the pen

namespace madc {
namespace hub {

struct term_screen
{
    enum class esc_state : unsigned char { normal, esc, csi, osc, osc_esc };

    std::vector<std::string> lines;	// completed lines, oldest first
    std::string cur;			// the line the cursor is on
    size_t col;				// the cursor column in `cur`
    esc_state st;
    std::string params;			// the CSI parameter bytes so far
    size_t cap;				// the scrollback: completed lines kept
    // The pen and every byte's style (S8): an id per byte, parallel to the
    // text, into a palette of the distinct styles written (id 0 = the
    // default; a screen with more than 255 styles writes the rest plain).
    ui_style pen;
    std::vector<ui_style> pens;
    std::vector<std::string> line_ids;	// parallel to `lines`
    std::string cur_ids;		// parallel to `cur`

    // One coloured run of the text: bytes [s, e), its style as a spec.
    struct span { size_t s, e; std::string spec; };

    term_screen() : col(0), st(esc_state::normal), cap(5000), pens(1) {}

    // Load from the text an owner kept (lines joined by '\n'; the last
    // segment is the current line) and the cursor column it kept.
    void load(const std::string &text, size_t column)
    {
	lines.clear();
	cur.clear();
	size_t start = 0;
	for ( size_t i = 0; i < text.size(); ++i )
	    if ( text[i] == '\n' )
	    {
		lines.push_back(text.substr(start, i - start));
		start = i + 1;
	    }
	cur = text.substr(start);
	col = column <= cur.size() ? column : cur.size();
	line_ids.clear();
	for ( size_t i = 0; i < lines.size(); ++i )
	    line_ids.push_back(std::string(lines[i].size(), '\0'));
	cur_ids.assign(cur.size(), '\0');
    }
    // The spans an owner kept (spans()'s rows over the loaded text): each
    // byte of a run takes its style again. A spec the parser refuses, or a
    // run past the text, is skipped.
    void load_spans(const std::vector<span> &runs)
    {
	for ( size_t k = 0; k < runs.size(); ++k )
	{
	    ui_style st_;
	    if ( !ui_style_of(runs[k].spec, st_) )
		continue;
	    const char id = intern(st_);
	    size_t off = 0;
	    for ( size_t li = 0; li <= lines.size(); ++li )
	    {
		std::string &ids = li < lines.size() ? line_ids[li] : cur_ids;
		const size_t len = ids.size();
		for ( size_t b = 0; b < len; ++b )
		    if ( off + b >= runs[k].s && off + b < runs[k].e )
			ids[b] = id;
		off += len + 1;
		if ( off > runs[k].e )
		    break;
	    }
	}
    }
    // The pen an owner kept, as its spec ("" or a refused spec: the default).
    void load_pen(const std::string &spec)
    {
	ui_style p;
	pen = !spec.empty() && ui_style_of(spec, p) ? p : ui_style();
    }
    std::string pen_spec() const { return ui_style_spec(pen); }

    // The coloured runs over text(): one row per run of bytes written with
    // the same non-default style.
    std::vector<span> spans() const
    {
	std::vector<span> out;
	size_t off = 0;
	for ( size_t li = 0; li <= lines.size(); ++li )
	{
	    const std::string &ids = li < lines.size() ? line_ids[li] : cur_ids;
	    size_t b = 0;
	    while ( b < ids.size() )
	    {
		const unsigned char id = (unsigned char)ids[b];
		size_t e = b + 1;
		while ( e < ids.size() && ids[e] == ids[b] )
		    ++e;
		if ( id != 0 && id < pens.size() )
		{
		    span sp;
		    sp.s = off + b;
		    sp.e = off + e;
		    sp.spec = ui_style_spec(pens[id]);
		    out.push_back(sp);
		}
		b = e;
	    }
	    off += ids.size() + 1;
	}
	return out;
    }

    // The screen as one text: the completed lines, then the current one.
    std::string text() const
    {
	std::string out;
	for ( size_t i = 0; i < lines.size(); ++i )
	{
	    out += lines[i];
	    out += '\n';
	}
	out += cur;
	return out;
    }

    void clear()
    {
	lines.clear();
	cur.clear();
	line_ids.clear();
	cur_ids.clear();
	col = 0;
    }

    void feed(const char *bytes, size_t n)
    {
	for ( size_t i = 0; i < n; ++i )
	    feed_byte((unsigned char)bytes[i]);
    }

private:
    void newline()
    {
	lines.push_back(cur);
	line_ids.push_back(cur_ids);
	if ( lines.size() > cap )
	{
	    lines.erase(lines.begin(), lines.begin() + (lines.size() - cap));
	    line_ids.erase(line_ids.begin(), line_ids.begin() + (line_ids.size() - cap));
	}
	cur.clear();
	cur_ids.clear();
	col = 0;
    }
    // The palette id of a style (interned; 0 = the default, and every style
    // past the palette's 255).
    char intern(const ui_style &s_)
    {
	if ( s_ == ui_style() )
	    return 0;
	for ( size_t i = 1; i < pens.size(); ++i )
	    if ( pens[i] == s_ )
		return (char)i;
	if ( pens.size() >= 256 )
	    return 0;
	pens.push_back(s_);
	return (char)(pens.size() - 1);
    }
    // SGR (CSI ... m): the pen. 0 resets; 1 2 3 4 5 7 set bold, dim,
    // italic, underline, blink, inverse and 22 23 24 25 27 clear them; 30-37
    // / 40-47 a colour, 90-97 / 100-107 its bright twin, 38 / 48 with 5;n
    // (the 256 palette) or 2;r;g;b (24-bit), 39 / 49 the default.
    void sgr()
    {
	std::vector<int> p;
	size_t at = 0;
	for (;;)
	{
	    size_t semi = params.find_first_of(";:", at);
	    p.push_back(atoi(params.substr(at, semi == std::string::npos
					       ? std::string::npos : semi - at).c_str()));
	    if ( semi == std::string::npos )
		break;
	    at = semi + 1;
	}
	for ( size_t i = 0; i < p.size(); ++i )
	{
	    const int n = p[i];
	    if ( n == 0 )		pen = ui_style();
	    else if ( n == 1 )		pen.flags |= ui_style::BOLD;
	    else if ( n == 2 )		pen.flags |= ui_style::DIM;
	    else if ( n == 3 )		pen.flags |= ui_style::ITALIC;
	    else if ( n == 4 )		pen.flags |= ui_style::UNDERLINE;
	    else if ( n == 5 )		pen.flags |= ui_style::BLINK;
	    else if ( n == 7 )		pen.flags |= ui_style::INVERSE;
	    else if ( n == 22 )		pen.flags &= (unsigned char)~(ui_style::BOLD | ui_style::DIM);
	    else if ( n == 23 )		pen.flags &= (unsigned char)~ui_style::ITALIC;
	    else if ( n == 24 )		pen.flags &= (unsigned char)~ui_style::UNDERLINE;
	    else if ( n == 25 )		pen.flags &= (unsigned char)~ui_style::BLINK;
	    else if ( n == 27 )		pen.flags &= (unsigned char)~ui_style::INVERSE;
	    else if ( n >= 30 && n <= 37 )  set_index(false, n - 30);
	    else if ( n >= 40 && n <= 47 )  set_index(true, n - 40);
	    else if ( n >= 90 && n <= 97 )  set_rgb(false, ui_xterm256_rgb(n - 90 + 8));
	    else if ( n >= 100 && n <= 107 ) set_rgb(true, ui_xterm256_rgb(n - 100 + 8));
	    else if ( n == 39 )		{ pen.fg = 0; pen.fg_rgb = 0; }
	    else if ( n == 49 )		{ pen.bg = 0; pen.bg_rgb = 0; }
	    else if ( (n == 38 || n == 48) && i + 1 < p.size() )
	    {
		const bool bg = n == 48;
		if ( p[i + 1] == 5 && i + 2 < p.size() )
		{
		    const int c = p[i + 2];
		    if ( c >= 0 && c < 8 )
			set_index(bg, c);
		    else
			set_rgb(bg, ui_xterm256_rgb(c));
		    i += 2;
		}
		else if ( p[i + 1] == 2 && i + 4 < p.size() )
		{
		    set_rgb(bg, ((uint32_t)(p[i + 2] & 0xff) << 16)
				| ((uint32_t)(p[i + 3] & 0xff) << 8)
				| (uint32_t)(p[i + 4] & 0xff));
		    i += 4;
		}
		else
		    break;			// malformed: the rest is dropped
	    }
	}
    }
    // An ANSI colour 0..7 (black..white) or an exact one, as the theme
    // parser builds them (the value and its nearest 8-colour index).
    void set_index(bool bg, int c)
    {
	(bg ? pen.bg : pen.fg) = (unsigned char)(c + 1);
	(bg ? pen.bg_rgb : pen.fg_rgb) = 0;
    }
    void set_rgb(bool bg, uint32_t rgb)
    {
	(bg ? pen.bg : pen.fg) = ui_rgb_nearest_ansi(rgb);
	(bg ? pen.bg_rgb : pen.fg_rgb) = ui_style::RGB_SET | rgb;
    }
    void put(char c)
    {
	const char id = intern(pen);
	if ( col < cur.size() )
	{
	    cur[col] = c;
	    cur_ids[col] = id;
	}
	else
	{
	    if ( col > cur.size() )
	    {
		cur.append(col - cur.size(), ' ');
		cur_ids.append(col - cur_ids.size(), '\0');
	    }
	    cur += c;
	    cur_ids += id;
	}
	++col;
    }
    void csi_final(char final_byte)
    {
	switch ( final_byte )
	{
	    case 'K':		// erase in line: to the end (0 / none); the
				// start or the whole line spell 1 / 2
		if ( params == "1" )
		{
		    const size_t n = col < cur.size() ? col : cur.size();
		    cur.replace(0, n, n, ' ');
		    cur_ids.replace(0, n, n, '\0');
		}
		else if ( params == "2" )
		{
		    cur.assign(cur.size(), ' ');
		    cur_ids.assign(cur_ids.size(), '\0');
		}
		else if ( col < cur.size() )
		{
		    cur.erase(col);
		    cur_ids.erase(col);
		}
		break;
	    case 'J':		// erase in display: 2 / 3 = the whole screen
		if ( params == "2" || params == "3" )
		    clear();
		break;
	    case 'm':		// SGR: the pen (S8)
		sgr();
		break;
	    default:		// cursor motion, modes: dropped
		break;
	}
    }
    void feed_byte(unsigned char b)
    {
	switch ( st )
	{
	    case esc_state::esc:
		if ( b == '[' )	    { st = esc_state::csi; params.clear(); return; }
		if ( b == ']' )	    { st = esc_state::osc; return; }
		st = esc_state::normal;	// ESC + one byte (charset, keypad…): dropped
		return;
	    case esc_state::csi:
		if ( b >= 0x40 && b <= 0x7e )
		{
		    csi_final((char)b);
		    st = esc_state::normal;
		}
		else if ( params.size() < 32 )
		    params += (char)b;
		else
		    st = esc_state::normal;	// runaway: dropped
		return;
	    case esc_state::osc:
		if ( b == 0x07 )
		    st = esc_state::normal;
		else if ( b == 0x1b )
		    st = esc_state::osc_esc;
		return;
	    case esc_state::osc_esc:
		st = esc_state::normal;		// ESC \ ends it; anything else too
		return;
	    case esc_state::normal:
	    default:
		break;
	}
	switch ( b )
	{
	    case 0x1b: st = esc_state::esc; return;
	    case '\n': newline(); return;
	    case '\r': col = 0; return;
	    case '\b': if ( col > 0 ) --col; return;
	    case '\t':
	    {
		size_t next = (col / 8 + 1) * 8;
		while ( col < next )
		    put(' ');
		return;
	    }
	    default:
		if ( b < 0x20 || b == 0x7f )
		    return;			// BEL and the other controls: dropped
		put((char)b);
		return;
	}
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_TERM_SCREEN_H
