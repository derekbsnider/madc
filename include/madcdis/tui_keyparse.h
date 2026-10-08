#ifndef __MADCDIS_TUI_KEYPARSE_H
#define __MADCDIS_TUI_KEYPARSE_H 1

// madcdis/tui_keyparse.h — raw terminal bytes -> keys (split out of
// tui_model.h, TUI facelift S0): the escape-sequence state machine, the
// function keys' one tilde-code table and their bytes back out. The key
// VOCABULARY and spelling stay the key owner's (madcdis/keys.h).
//
// THREAD-SAFETY CONTRACT: a parser is a plain object confined to the
// thread that feeds it.

#include <cstdint>
#include <string>
#include <vector>

#include "madcdis/keys.h"		// tui_key, tui_keyev — the one key vocabulary

namespace madc {
namespace hub {

// Raw terminal bytes -> keys: the escape-sequence state machine (CSI and
// SS3 forms of the VT100/xterm family; the shapes every terminal library
// parses — cross-checked against termbox2's and ncurses's tables). A bare
// ESC is ambiguous until the input pauses: the TARGET calls flush() when
// its read times out after an ESC, resolving it to the esc key. xterm's
// modified keys decode with their modifiers (plan §41.11a step 3e): a
// cursor or function key's second parameter ("CSI 1;5A" Ctrl+Up,
// "CSI 15;2~" Shift+F5, "CSI 1;3P" Alt+F1), CSI Z (Shift+Tab), and a
// modifyOtherKeys or CSI u report of any other key ("CSI 27;6;83~",
// "CSI 115;5u"). ui::key_mod's bits are the parameter less one. A terminal
// that reports none sends Ctrl+Shift+S as Ctrl+S; those chords are the
// GUI's. NUL is Ctrl+Space. An Esc-prefixed byte is still the esc key then
// the byte (the profiles' `esc x` Meta chords).
//
// A byte of 0x80 and above is a `ch`: UTF-8 input arrives as the bytes of
// its code points, and a printable run coalesces them into one text event
// (plan §41.7a). A BRACKETED PASTE (xterm's mode 2004: CSI 200~ ... CSI
// 201~, which a target turns on only where it wants it) is text, not keys:
// every byte between the markers is a literal `ch`, a tab and a line break
// included, and a CR or CR LF becomes one '\n'. A paste spans reads; only
// its end marker ends it.
// The function keys' xterm tilde codes (CSI n ~), F1..F12: ONE table the
// parser and tui_key_bytes read. F1..F4 also arrive as SS3 P..S (xterm's
// own spelling for them, which tui_key_bytes writes back), and F1..F5 as
// the Linux console's CSI [ A..E.
inline int fkey_tilde_code(int n)
{
    static const int codes[13] = { 0, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24 };
    return n >= 1 && n <= 12 ? codes[n] : 0;
}
inline int fkey_of_tilde_code(int code)
{
    for ( int n = 1; n <= 12; ++n )
	if ( fkey_tilde_code(n) == code )
	    return n;
    return 0;
}

class tui_keyparse
{
    enum class state : unsigned char { normal, esc, csi, ss3, console_fkey, paste };
    state _st;
    std::string _params;
    std::string _paste_end;	// the part of the end marker matched so far
    bool _paste_cr;		// the last pasted byte was CR (CR LF is one)

    static void emit(std::vector<tui_keyev> &out, tui_key k, char c = 0,
		     unsigned char mods = 0)
    {
	out.push_back(key_normalized(tui_keyev(k, c, mods)));
    }
    // A key a modifyOtherKeys / CSI u report names by its code.
    static void emit_code(std::vector<tui_keyev> &out, int code,
			  unsigned char mods)
    {
	switch ( code )
	{
	    case 9:   emit(out, tui_key::tab, 0, mods); return;
	    case 13:  emit(out, tui_key::enter, 0, mods); return;
	    case 27:  emit(out, tui_key::esc, 0, mods); return;
	    case 8:
	    case 127: emit(out, tui_key::backspace, 0, mods); return;
	    default:
		if ( code >= 0x20 && code <= 0x7e )
		    emit(out, tui_key::ch, (char)code, mods);
		return;			// otherwise unrecognized: dropped
	}
    }
    static void resolve_csi(const std::string &params, char final_byte,
			    std::vector<tui_keyev> &out)
    {
	// "a;b;c": the key (or 1), the modifiers + 1, a code.
	int p[3] = { 0, 0, 0 };
	size_t np = 0, at = 0;
	while ( np < 3 )
	{
	    size_t semi = params.find(';', at);
	    p[np++] = atoi(params.substr(at, semi == std::string::npos
					       ? std::string::npos : semi - at).c_str());
	    if ( semi == std::string::npos )
		break;
	    at = semi + 1;
	}
	unsigned char mods = p[1] > 1 ? (unsigned char)(p[1] - 1) : 0;
	switch ( final_byte )
	{
	    case 'A': emit(out, tui_key::up, 0, mods); return;
	    case 'B': emit(out, tui_key::down, 0, mods); return;
	    case 'C': emit(out, tui_key::right, 0, mods); return;
	    case 'D': emit(out, tui_key::left, 0, mods); return;
	    case 'H': emit(out, tui_key::home, 0, mods); return;
	    case 'F': emit(out, tui_key::end, 0, mods); return;
	    case 'P': case 'Q': case 'R': case 'S':	// CSI 1;m P: a modified F1..F4
		emit(out, tui_key::fkey, (char)(final_byte - 'P' + 1), mods);
		return;
	    case 'Z':				// back-tab
		emit(out, tui_key::tab, 0,
		     (unsigned char)(mods | key_mod_bits(::ui::key_mod::shift)));
		return;
	    case 'u':				// CSI code;m u
		emit_code(out, p[0], mods);
		return;
	    case '~':
		switch ( p[0] )
		{
		    case 1: case 7: emit(out, tui_key::home, 0, mods); return;
		    case 4: case 8: emit(out, tui_key::end, 0, mods); return;
		    case 2: emit(out, tui_key::ins, 0, mods); return;
		    case 3: emit(out, tui_key::del, 0, mods); return;
		    case 5: emit(out, tui_key::pgup, 0, mods); return;
		    case 6: emit(out, tui_key::pgdn, 0, mods); return;
		    case 27:			// modifyOtherKeys: 27;m;code
			emit_code(out, p[2], mods);
			return;
		    default:
		    {
			int n = fkey_of_tilde_code(p[0]);
			if ( n )
			    emit(out, tui_key::fkey, (char)n, mods);
			return;		// otherwise unrecognized: dropped
		    }
		}
	    default: return;		// unrecognized final: dropped
	}
    }
    // One pasted byte as text: a line break is '\n' whatever the terminal
    // sent for it.
    void paste_byte(unsigned char b, std::vector<tui_keyev> &out)
    {
	bool cr = _paste_cr;
	_paste_cr = b == '\r';
	if ( b == '\n' && cr )
	    return;			// the LF of a CR LF
	emit(out, tui_key::ch, b == '\r' ? '\n' : (char)b);
    }
    void feed_byte(unsigned char b, std::vector<tui_keyev> &out)
    {
	switch ( _st )
	{
	    case state::paste:
	    {
		static const char end_marker[] = "\x1b[201~";
		if ( b == (unsigned char)end_marker[_paste_end.size()] )
		{
		    _paste_end += (char)b;
		    if ( _paste_end.size() == sizeof(end_marker) - 1 )
		    {
			_paste_end.clear();
			_st = state::normal;
		    }
		    return;
		}
		// A partial marker that went no further was pasted text; the
		// byte that broke it may begin a marker itself.
		if ( !_paste_end.empty() )
		{
		    std::string held;
		    held.swap(_paste_end);
		    for ( size_t i = 0; i < held.size(); ++i )
			paste_byte((unsigned char)held[i], out);
		    feed_byte(b, out);
		    return;
		}
		paste_byte(b, out);
		return;
	    }
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
		if ( b == '[' && _params.empty() )
		{
		    _st = state::console_fkey;	// the Linux console's F1..F5
		    return;
		}
		if ( b >= 0x40 && b <= 0x7e )
		{
		    if ( b == '~' && _params == "200" )
		    {
			_st = state::paste;	// a bracketed paste begins
			_paste_end.clear();
			_paste_cr = false;
			return;
		    }
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
		    case 'P': case 'Q': case 'R': case 'S':
			emit(out, tui_key::fkey, (char)(b - 'P' + 1));
			break;
		    default: break;		// unrecognized: dropped
		}
		_st = state::normal;
		return;
	    case state::console_fkey:
		if ( b >= 'A' && b <= 'E' )
		    emit(out, tui_key::fkey, (char)(b - 'A' + 1));
		_st = state::normal;		// anything else: dropped
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
	else if ( b >= 0x20 )
	    emit(out, tui_key::ch, (char)b);	// ASCII, and UTF-8's bytes
	else if ( b == 0x00 )			// the terminals' Ctrl+Space
	    emit(out, tui_key::ch, ' ', key_mod_bits(::ui::key_mod::ctrl));
	// A grid still draws one byte per cell, so a multibyte glyph there is
	// the grid's named residue.
    }

public:
    tui_keyparse() : _st(state::normal), _paste_cr(false) {}

    void feed(const char *bytes, size_t n, std::vector<tui_keyev> &out)
    {
	for ( size_t i = 0; i < n; ++i )
	    feed_byte((unsigned char)bytes[i], out);
    }
    // Mid-sequence? The target polls briefly only then — an unambiguous
    // batch pays zero added latency. A paste is not pending: it spans
    // reads, and a pause inside it waits for no grace read.
    bool pending() const
	{ return _st != state::normal && _st != state::paste; }
    // The input paused: a pending bare ESC is the esc key; a partial
    // CSI/SS3 is line noise and drops. A paste keeps going.
    void flush(std::vector<tui_keyev> &out)
    {
	if ( _st == state::paste )
	    return;
	if ( _st == state::esc )
	    emit(out, tui_key::esc);
	_st = state::normal;
	_params.clear();
    }
};

// The INVERSE adapter — a key back to the bytes a terminal would have sent
// for it (madcide polish P3b-2: what the IDE writes to a program running on
// its embedded Terminal's pty when the user types at it). ONE table with
// tui_keyparse above: every key the parser yields round-trips through
// these bytes (the unit battery pins it) — the xterm/VT100 spellings the
// parser's CSI/SS3 arms read (the CSI form for the cursor keys, the tilde
// codes for ins/del/pgup/pgdn, 0x7f for backspace, \r for enter). A
// control chord is its control byte; a printable is itself; `none` is
// empty. A modified key is xterm's modified form (the parser's): the cursor
// and function keys' "1;m" / "n;m" parameters, CSI Z for Shift+Tab,
// modifyOtherKeys (CSI 27;m;code~) for any other key, NUL for Ctrl+Space.
inline std::string tui_key_base_bytes(const tui_keyev &k);
inline std::string tui_key_bytes(const tui_keyev &k)
{
    if ( k.mods == 0 )
	return tui_key_base_bytes(k);
    const unsigned char ctrl = key_mod_bits(::ui::key_mod::ctrl);
    const unsigned char shift = key_mod_bits(::ui::key_mod::shift);
    unsigned char mods = k.mods;
    if ( k.kind == tui_key::ctrl )
	mods |= ctrl;
    const std::string m = std::to_string(1 + (int)mods);
    switch ( k.kind )
    {
	case tui_key::up:    return "\x1b[1;" + m + "A";
	case tui_key::down:  return "\x1b[1;" + m + "B";
	case tui_key::right: return "\x1b[1;" + m + "C";
	case tui_key::left:  return "\x1b[1;" + m + "D";
	case tui_key::home:  return "\x1b[1;" + m + "H";
	case tui_key::end:   return "\x1b[1;" + m + "F";
	case tui_key::ins:   return "\x1b[2;" + m + "~";
	case tui_key::del:   return "\x1b[3;" + m + "~";
	case tui_key::pgup:  return "\x1b[5;" + m + "~";
	case tui_key::pgdn:  return "\x1b[6;" + m + "~";
	case tui_key::fkey:
	    if ( k.ch >= 1 && k.ch <= 4 )
		return "\x1b[1;" + m + (char)('P' + k.ch - 1);
	    if ( k.ch >= 5 && k.ch <= 12 )
		return "\x1b[" + std::to_string(fkey_tilde_code(k.ch)) + ";" + m + "~";
	    return std::string();
	case tui_key::tab:
	    if ( k.mods == shift )
		return std::string("\x1b[Z");
	    return "\x1b[27;" + m + ";9~";
	case tui_key::enter:	 return "\x1b[27;" + m + ";13~";
	case tui_key::esc:	 return "\x1b[27;" + m + ";27~";
	case tui_key::backspace: return "\x1b[27;" + m + ";127~";
	case tui_key::ch:
	    if ( k.ch == ' ' && k.mods == ctrl )
		return std::string(1, '\0');
	    return "\x1b[27;" + m + ";" + std::to_string((int)(unsigned char)k.ch) + "~";
	case tui_key::ctrl:
	{
	    // The letter's code: upper-case under Shift, as xterm reports it.
	    char c = k.ch;
	    if ( (k.mods & shift) && c >= 'a' && c <= 'z' )
		c = (char)(c - 'a' + 'A');
	    return "\x1b[27;" + m + ";" + std::to_string((int)(unsigned char)c) + "~";
	}
	default:		 return std::string();
    }
}
inline std::string tui_key_base_bytes(const tui_keyev &k)
{
    switch ( k.kind )
    {
	case tui_key::ch:	 return std::string(1, k.ch);
	case tui_key::ctrl:
	    if ( k.ch >= 'a' && k.ch <= 'z' )
		return std::string(1, (char)(k.ch - 'a' + 1));
	    if ( k.ch >= '\\' && k.ch <= '_' )		// ^\ ^] ^^ ^_
		return std::string(1, (char)(k.ch - 0x40));
	    return std::string();
	case tui_key::enter:	 return std::string("\r");
	case tui_key::tab:	 return std::string("\t");
	case tui_key::backspace: return std::string("\x7f");
	case tui_key::esc:	 return std::string("\x1b");
	case tui_key::up:	 return std::string("\x1b[A");
	case tui_key::down:	 return std::string("\x1b[B");
	case tui_key::right:	 return std::string("\x1b[C");
	case tui_key::left:	 return std::string("\x1b[D");
	case tui_key::home:	 return std::string("\x1b[H");
	case tui_key::end:	 return std::string("\x1b[F");
	case tui_key::ins:	 return std::string("\x1b[2~");
	case tui_key::del:	 return std::string("\x1b[3~");
	case tui_key::pgup:	 return std::string("\x1b[5~");
	case tui_key::pgdn:	 return std::string("\x1b[6~");
	case tui_key::fkey:
	    if ( k.ch >= 1 && k.ch <= 4 )
		return std::string("\x1bO") + (char)('P' + k.ch - 1);
	    if ( k.ch >= 5 && k.ch <= 12 )
		return "\x1b[" + std::to_string(fkey_tilde_code(k.ch)) + "~";
	    return std::string();
	default:		 return std::string();
    }
}


} // namespace hub
} // namespace madc

#endif // __MADCDIS_TUI_KEYPARSE_H
