#ifndef __MADCDIS_TEXT_UTF16_H
#define __MADCDIS_TEXT_UTF16_H 1

// madcdis/text_utf16.h — THE owner of UTF-16 ↔ UTF-8 column arithmetic over
// one line of text.
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

} // namespace madc

#endif // __MADCDIS_TEXT_UTF16_H
