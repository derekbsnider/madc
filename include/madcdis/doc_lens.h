#ifndef __MADCDIS_DOC_LENS_H
#define __MADCDIS_DOC_LENS_H 1

// madcdis/doc_lens.h — the document-lens coordinate map (madcide AST-3;
// design: docs/plans/2026-08-25-madcide-ast-arc-design.md §2 "View
// switching"). THE seam between what an editor DISPLAYS and what the
// document STORES: a lens computes display text from stored text (get),
// and this map — ordered copy segments, a first-class data structure —
// carries the display↔stored byte correspondence beside it. Caret and
// selection TRUTH stays in stored offsets on the bag; projection through
// the map is the ONE place display coordinates come from — never per-view
// arithmetic (the recurring caret-math failure mode this exists to
// prevent).
//
// Vocabulary:
//  - a COPY segment {disp, stored, len}: len bytes present in both spaces;
//  - a stored-side gap between segments: CONCEALED bytes (stored, not
//    displayed — markdown formatting characters, folded regions);
//  - a display-side gap: SYNTHETIC bytes (displayed decoration with no
//    stored home — rendered code views, conceal replacement glyphs);
//  - the identity lens = one segment [0,n)↔[0,n), or no map at all;
//  - a wholly rendered view (MC11 / C11) = no segments (all synthetic).
//
// Projection contract (pinned by tests/unit/test_doc_lens.cpp): offsets
// are CARET positions (0..len are all valid). An offset inside a copy
// segment (its end included) maps 1:1; STRICTLY inside a gap it collapses
// FORWARD to the next copy segment's start in the other space; past the
// last segment it lands just after that segment's image (trailing
// concealed/synthetic bytes never attract the caret); an EMPTY map
// answers 0 (nothing corresponds — park at the top). A boundary offset
// shared by a copy segment's END and a gap-following segment's START
// belongs to the copy that ends there, so the inverse of a gap-adjacent
// caret lands at the EARLIER position — outside the concealed run, the
// safe side for a future put.
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): a plain value
// object — build, then read; confined per the C++ stdlib convention.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "libmadc/value.h"

namespace madc {
namespace hub {

class doc_map
{
public:
    struct seg { size_t disp, stored, len; };

    // Monotone append: a segment must be non-empty and start at or after
    // BOTH ends of the previous one. False = refused, the map unchanged.
    bool add(size_t disp, size_t stored, size_t len)
    {
	if ( len == 0 )
	    return false;
	if ( !_segs.empty() )
	{
	    const seg &p = _segs.back();
	    if ( disp < p.disp + p.len || stored < p.stored + p.len )
		return false;
	}
	seg s;
	s.disp = disp;
	s.stored = stored;
	s.len = len;
	_segs.push_back(s);
	return true;
    }

    size_t seg_count() const { return _segs.size(); }
    bool empty() const { return _segs.empty(); }

    // stored caret offset -> display caret offset.
    size_t to_display(size_t stored) const { return project(stored, false); }
    // display caret offset -> stored caret offset (the put direction's
    // coordinate half: where a display-space event lands in the store).
    size_t to_stored(size_t disp) const { return project(disp, true); }

    // The map AS data (spans-as-data, exactly like the projection hints):
    // an array of {disp, stored, len} integer rows.
    madc::value to_value() const
    {
	std::vector<madc::value> rows;
	for ( size_t i = 0; i < _segs.size(); ++i )
	{
	    std::map<std::string, madc::value> f;
	    f["disp"] = madc::value((int64_t)_segs[i].disp);
	    f["stored"] = madc::value((int64_t)_segs[i].stored);
	    f["len"] = madc::value((int64_t)_segs[i].len);
	    rows.push_back(madc::value::make_object(f));
	}
	return madc::value::make_array(rows);
    }

    // Strict reader: a malformed or non-monotone row REFUSES the whole
    // map (a coordinate map is a contract; a silently skipped row would
    // corrupt caret math). False = out left empty.
    static bool from_value(const madc::value &v, doc_map &out)
    {
	out = doc_map();
	if ( !v.is_array() )
	    return false;
	const std::vector<madc::value> &rows = v.as_array();
	for ( size_t i = 0; i < rows.size(); ++i )
	{
	    int64_t d, s, n;
	    if ( !row_field(rows[i], "disp", d)
	      || !row_field(rows[i], "stored", s)
	      || !row_field(rows[i], "len", n) )
	    {
		out = doc_map();
		return false;
	    }
	    if ( !out.add((size_t)d, (size_t)s, (size_t)n) )
	    {
		out = doc_map();
		return false;
	    }
	}
	return true;
    }

private:
    std::vector<seg> _segs;

    static bool row_field(const madc::value &row, const char *key,
			  int64_t &out)
    {
	if ( !row.is_object() )
	    return false;
	const std::map<std::string, madc::value> &o = row.as_object();
	std::map<std::string, madc::value>::const_iterator it = o.find(key);
	if ( it == o.end() || !it->second.is_integer() )
	    return false;
	out = it->second.as_integer();
	return out >= 0;
    }

    // The one projection rule, both directions. `from_disp` selects which
    // axis `from` lives on; the answer is on the other axis.
    size_t project(size_t from, bool from_disp) const
    {
	if ( _segs.empty() )
	    return 0;
	for ( size_t i = 0; i < _segs.size(); ++i )
	{
	    const seg &s = _segs[i];
	    size_t a = from_disp ? s.disp : s.stored;
	    size_t b = from_disp ? s.stored : s.disp;
	    if ( from < a )
		return b;		// strictly inside a gap: forward
	    if ( from <= a + s.len )
		return b + (from - a);	// inside the copy (end included)
	}
	const seg &l = _segs.back();
	return (from_disp ? l.stored : l.disp) + l.len;
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_DOC_LENS_H
