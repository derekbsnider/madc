#ifndef __MADCDIS_TEXT_UTF16_H
#define __MADCDIS_TEXT_UTF16_H 1

// madcdis/text_utf16.h — THE owner of UTF-16 ↔ UTF-8 column arithmetic over
// one line of text, and of a code point's DISPLAY width (the line editor's
// caret, plan §41.7a).
//
// Two consumers ask the same question from opposite directions:
//   - the web model (V3c): a page's hit test reports a JavaScript string
//     index, which is a UTF-16 code-unit column, and the buffer stores bytes;
//   - the LSP face (V6c-2): the protocol's DEFAULT position encoding is
//     UTF-16 code units, and every position the server reads or reports
//     crosses the same boundary.
//
// One code point below U+10000 is one UTF-16 unit; a four-byte UTF-8 sequence
// is a surrogate PAIR, so two units. A stray continuation byte counts as one
// unit of one byte — a malformed line still maps monotonically instead of
// throwing the caller off the end.
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): pure functions of
// their arguments.

#include <cstddef>
#include <cstdint>
#include <string>

namespace madc {

// The number of bytes one UTF-8 lead byte begins.
inline std::size_t utf8_seq_len(unsigned char lead)
{
	return lead < 0x80 ? 1 : lead < 0xC0 ? 1 : lead < 0xE0 ? 2
	     : lead < 0xF0 ? 3 : 4;		// a stray continuation byte: one
}

// A UTF-16 column → the byte index in the line's UTF-8 text. A column past
// the end clamps to the line's length; one landing inside a surrogate pair
// snaps to the pair's start (the only honest answer — half a code point is
// not a position).
inline std::size_t col16_to_byte(const std::string &t, long col16)
{
	if ( col16 <= 0 )
		return 0;
	std::size_t i = 0;
	long units = 0;
	while ( i < t.size() && units < col16 )
	{
		std::size_t n = utf8_seq_len((unsigned char)t[i]);
		if ( i + n > t.size() )
			n = t.size() - i;
		units += n == 4 ? 2 : 1;
		if ( units > col16 )
			break;			// inside a pair: snap to its start
		i += n;
	}
	return i;
}

// The inverse: a byte index → the UTF-16 column. A byte index past the end
// clamps to the line's full width; one inside a sequence counts that sequence
// as not yet reached (the same snap, from the other side).
inline long col16_of_byte(const std::string &t, std::size_t bytecol)
{
	if ( bytecol > t.size() )
		bytecol = t.size();
	std::size_t i = 0;
	long units = 0;
	while ( i < bytecol )
	{
		std::size_t n = utf8_seq_len((unsigned char)t[i]);
		if ( i + n > t.size() )
			n = t.size() - i;
		if ( i + n > bytecol )
			break;			// a partial sequence: not reached
		units += n == 4 ? 2 : 1;
		i += n;
	}
	return units;
}

// The code point at byte `i` of `t`, and the number of bytes it spans. A
// malformed or truncated sequence is one byte whose code point is that byte
// (utf8_seq_len's stray-byte rule), so a scan always advances.
inline std::size_t utf8_decode_at(const std::string &t, std::size_t i,
				  uint32_t &cp)
{
	unsigned char b = (unsigned char)t[i];
	std::size_t n = utf8_seq_len(b);
	if ( n == 1 || i + n > t.size() )
	{
		cp = b;
		return 1;
	}
	uint32_t v = n == 2 ? (b & 0x1Fu) : n == 3 ? (b & 0x0Fu) : (b & 0x07u);
	for ( std::size_t k = 1; k < n; ++k )
	{
		unsigned char c = (unsigned char)t[i + k];
		if ( (c & 0xC0) != 0x80 )
		{
			cp = b;
			return 1;
		}
		v = (v << 6) | (c & 0x3Fu);
	}
	cp = v;
	return n;
}

// A code point's display width in columns: the C++20 standard's estimated
// width, the rule std::format's fill and alignment use ([format.string.std]
// /11, P1868). The East Asian wide and fullwidth blocks and the emoji ranges
// it lists are 2 columns; every other code point is 1. It is an estimate in
// the standard's own words: there is no grapheme clustering, so a combining
// mark counts one column. C++23 names the Unicode East_Asian_Width property
// instead, whose table this list approximates.
inline unsigned codepoint_columns(uint32_t cp)
{
	static const uint32_t wide[][2] = {
		{ 0x1100, 0x115F },   { 0x2329, 0x232A },   { 0x2E80, 0x303E },
		{ 0x3040, 0xA4CF },   { 0xAC00, 0xD7A3 },   { 0xF900, 0xFAFF },
		{ 0xFE10, 0xFE19 },   { 0xFE30, 0xFE6F },   { 0xFF00, 0xFF60 },
		{ 0xFFE0, 0xFFE6 },   { 0x1F300, 0x1F64F }, { 0x1F900, 0x1F9FF },
		{ 0x20000, 0x2FFFD }, { 0x30000, 0x3FFFD },
	};
	for ( std::size_t i = 0; i < sizeof(wide) / sizeof(wide[0]); ++i )
		if ( cp >= wide[i][0] && cp <= wide[i][1] )
			return 2;
	return 1;
}

} // namespace madc

#endif // __MADCDIS_TEXT_UTF16_H
