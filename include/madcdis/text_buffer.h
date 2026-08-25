#ifndef __MADCDIS_TEXT_BUFFER_H
#define __MADCDIS_TEXT_BUFFER_H 1

// madcdis/text_buffer.h — the piece-table text component (Track 8.1 pulled
// forward per the hub doc's Phase-2 note; Track 7.2 R4): the hub's SECOND
// component kind after the value bag. "An editor buffer = entity with a
// piece-table component, natively" (design demand 7 — granularity below
// the record).
//
// A piece table: the LOADED text is an immutable snapshot, every insert
// appends to an add-only buffer, and the live document is a sequence of
// PIECES pointing into the two. Edits never move loaded bytes — an insert
// or erase splits/trims pieces, so edit cost is in pieces, not in document
// bytes, and (later, madcide) undo is a pieces-vector snapshot.
//
// Scale contract (hub.h's words): scans are linear and that is the
// CONTRACT — line/find queries walk the pieces in one pass; indexing is a
// later, measured change. Offsets are BYTE offsets; a line is the span
// between newlines, its length EXCLUDING the '\n' (a trailing unterminated
// span is a line; an empty buffer has zero lines — the ed model).
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): a plain value
// object, confined to the thread that owns it (the hub world's contract).

#include <cstddef>
#include <string>
#include <vector>

namespace madc {
namespace hub {

class text_buffer
{
public:
    // Fixed-underlying-type enum, not a static const member: an enumerator
    // is an rvalue, so header-only use never ODR-needs an out-of-line
    // definition (C++11 has no inline variables).
    enum : size_t { npos = (size_t)-1 };

    text_buffer() : _size(0) {}

    // Reset to a single piece over a fresh immutable snapshot.
    void load(const std::string &s)
    {
	_original = s;
	_add.clear();
	_pieces.clear();
	if ( !_original.empty() )
	{
	    piece p;
	    p.add = false;
	    p.off = 0;
	    p.len = _original.size();
	    _pieces.push_back(p);
	}
	_size = _original.size();
    }

    size_t size() const { return _size; }

    // Materialize the live document.
    std::string text() const
    {
	std::string out;
	out.reserve(_size);
	for ( size_t i = 0; i < _pieces.size(); ++i )
	    out.append(source(_pieces[i]), _pieces[i].off, _pieces[i].len);
	return out;
    }

    // Materialize [off, off+len) clamped to the document.
    std::string slice(size_t off, size_t len) const
    {
	std::string out;
	if ( off >= _size )
	    return out;
	if ( len > _size - off )
	    len = _size - off;
	out.reserve(len);
	size_t pos = 0;
	for ( size_t i = 0; i < _pieces.size() && len > 0; ++i )
	{
	    const piece &p = _pieces[i];
	    if ( off < pos + p.len )
	    {
		size_t skip = off > pos ? off - pos : 0;
		size_t take = p.len - skip;
		if ( take > len )
		    take = len;
		out.append(source(p), p.off + skip, take);
		off += take;
		len -= take;
	    }
	    pos += p.len;
	}
	return out;
    }

    // Insert `s` before offset `off` (clamped to the end). The text lands
    // in the add buffer; at most one existing piece splits.
    void insert(size_t off, const std::string &s)
    {
	if ( s.empty() )
	    return;
	if ( off > _size )
	    off = _size;
	piece np;
	np.add = true;
	np.off = _add.size();
	np.len = s.size();
	_add += s;
	size_t pos = 0;
	for ( size_t i = 0; i < _pieces.size(); ++i )
	{
	    piece &p = _pieces[i];
	    if ( off == pos )
	    {
		_pieces.insert(_pieces.begin() + i, np);
		_size += np.len;
		return;
	    }
	    if ( off < pos + p.len )
	    {
		piece tail = p;
		size_t head_len = off - pos;
		tail.off += head_len;
		tail.len -= head_len;
		p.len = head_len;
		_pieces.insert(_pieces.begin() + i + 1, tail);
		_pieces.insert(_pieces.begin() + i + 1, np);
		_size += np.len;
		return;
	    }
	    pos += p.len;
	}
	_pieces.push_back(np);
	_size += np.len;
    }

    // Erase [off, off+len) clamped to the document: trim, drop, or split
    // the pieces the range touches.
    void erase(size_t off, size_t len)
    {
	if ( off >= _size || len == 0 )
	    return;
	if ( len > _size - off )
	    len = _size - off;
	_size -= len;
	size_t pos = 0;
	for ( size_t i = 0; i < _pieces.size() && len > 0; )
	{
	    piece &p = _pieces[i];
	    size_t pend = pos + p.len;
	    if ( off >= pend )
	    {
		pos = pend;
		++i;
		continue;
	    }
	    size_t skip = off > pos ? off - pos : 0;
	    size_t cut = p.len - skip;
	    if ( cut > len )
		cut = len;
	    if ( skip == 0 && cut == p.len )
	    {
		// whole piece goes
		_pieces.erase(_pieces.begin() + i);
		len -= cut;
		continue;	// pos unchanged; same index now next piece
	    }
	    if ( skip == 0 )
	    {
		// front trim
		p.off += cut;
		p.len -= cut;
		len -= cut;
		pos += p.len;
		++i;
		continue;
	    }
	    if ( skip + cut == p.len )
	    {
		// back trim
		p.len = skip;
		len -= cut;
		pos += p.len;
		++i;
		continue;
	    }
	    // interior: split into head + tail
	    piece tail = p;
	    tail.off += skip + cut;
	    tail.len -= skip + cut;
	    p.len = skip;
	    _pieces.insert(_pieces.begin() + i + 1, tail);
	    len -= cut;
	    pos += p.len;
	    ++i;
	}
    }

    void replace(size_t off, size_t len, const std::string &s)
    {
	erase(off, len);
	insert(off, s);
    }

    // Lines: newline-delimited spans; a trailing unterminated span counts;
    // an empty buffer has zero lines.
    size_t line_count() const
    {
	size_t lines = 0;
	bool open = false;
	for ( size_t i = 0; i < _pieces.size(); ++i )
	{
	    const piece &p = _pieces[i];
	    const std::string &src = source(p);
	    for ( size_t k = 0; k < p.len; ++k )
	    {
		open = true;
		if ( src[p.off + k] == '\n' )
		{
		    ++lines;
		    open = false;
		}
	    }
	}
	return lines + (open ? 1 : 0);
    }

    // The span of 1-based line `n`: byte offset and length EXCLUDING the
    // terminating '\n'. False when the buffer has no such line.
    bool line_span(size_t n, size_t &off, size_t &len) const
    {
	if ( n == 0 )
	    return false;
	size_t line = 1;
	size_t start = 0;
	size_t pos = 0;
	for ( size_t i = 0; i < _pieces.size(); ++i )
	{
	    const piece &p = _pieces[i];
	    const std::string &src = source(p);
	    for ( size_t k = 0; k < p.len; ++k, ++pos )
	    {
		if ( src[p.off + k] != '\n' )
		    continue;
		if ( line == n )
		{
		    off = start;
		    len = pos - start;
		    return true;
		}
		++line;
		start = pos + 1;
	    }
	}
	if ( line == n && start < _size )
	{
	    off = start;
	    len = _size - start;
	    return true;
	}
	return false;
    }

    // First occurrence of `needle` at or after `from`; npos when absent.
    // Materializes — the documented linear-scan contract.
    size_t find(size_t from, const std::string &needle) const
    {
	if ( needle.empty() || from >= _size )
	    return npos;
	std::string t = text();
	size_t hit = t.find(needle, from);
	return hit == std::string::npos ? npos : hit;
    }

    size_t piece_count() const { return _pieces.size(); }	// unit-test view

private:
    struct piece
    {
	bool add;	// which source: add buffer or the loaded snapshot
	size_t off;
	size_t len;
    };

    const std::string &source(const piece &p) const
    {
	return p.add ? _add : _original;
    }

    std::string _original;	// the loaded snapshot — never mutated
    std::string _add;		// append-only insert storage
    std::vector<piece> _pieces;
    size_t _size;
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_TEXT_BUFFER_H
