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
// sequences (window titles) end at BEL or ESC \. SGR colours are dropped
// too (onto the one style later — a named residue). Byte-oriented: a
// multi-byte UTF-8 glyph occupies one column per byte, as the grid model's
// pilot does. The scrollback keeps at most `cap` completed lines.
//
// The state is a VALUE: the completed lines' text, the current line, the
// cursor column and the escape parser's state — so an owner may keep it
// on a document (the text + a few attributes) between feeds, feeding each
// chunk the pump reads. Dependency-free: <string> <vector>.
//
// Thread contract: a plain value object; confined with the pump that
// feeds it (the C++ standard-library convention).

#include <string>
#include <vector>

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

    term_screen() : col(0), st(esc_state::normal), cap(5000) {}

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
	if ( lines.size() > cap )
	    lines.erase(lines.begin(), lines.begin() + (lines.size() - cap));
	cur.clear();
	col = 0;
    }
    void put(char c)
    {
	if ( col < cur.size() )
	    cur[col] = c;
	else
	{
	    if ( col > cur.size() )
		cur.append(col - cur.size(), ' ');
	    cur += c;
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
		    cur.replace(0, col < cur.size() ? col : cur.size(),
				col < cur.size() ? col : cur.size(), ' ');
		else if ( params == "2" )
		    cur.assign(cur.size(), ' ');
		else if ( col < cur.size() )
		    cur.erase(col);
		break;
	    case 'J':		// erase in display: 2 / 3 = the whole screen
		if ( params == "2" || params == "3" )
		    clear();
		break;
	    default:		// cursor motion, SGR, modes: dropped
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
