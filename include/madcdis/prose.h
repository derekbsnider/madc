#ifndef __MADCDIS_PROSE_H
#define __MADCDIS_PROSE_H 1

// madcdis/prose.h — the projection library's sentence-building kit
// (Track 7 Phase 1, S3). PROSE IS CONTENT (design, Decided): composition
// lives here, beside the projections, never in a renderer — a level-3
// screen reader and a tooltip want the same sentence the teletype prints.
// The MUD string heritage: enumeration, articles, pluralization.
//
// The seam contract (design, Decided): composed prose is DERIVED and the
// facts behind it stay authoritative; authored prose is itself a fact and
// coexists. True NLG is the owner's stated ultimate goal and plugs in
// behind this same seam, post-Phase-1.
//
// THREAD-SAFETY CONTRACT: pure functions over their arguments.

#include <cstdio>
#include <string>
#include <vector>

#include "libmadc/value.h"

namespace madc {
namespace hub {
namespace prose {

// Render a value's content as display text: the projection-side coercion
// (string payload verbatim; scalars in their natural spelling; null empty).
inline std::string text_of(const madc::value &v)
{
    switch ( v.type() )
    {
	case madc::value::kind::string:
	    return v.as_string();
	case madc::value::kind::integer:
	{
	    char buf[32];
	    snprintf(buf, sizeof(buf), "%lld", (long long)v.as_integer());
	    return std::string(buf);
	}
	case madc::value::kind::real:
	{
	    char buf[48];
	    snprintf(buf, sizeof(buf), "%g", v.as_real());
	    return std::string(buf);
	}
	case madc::value::kind::boolean:
	    return v.as_boolean() ? "true" : "false";
	case madc::value::kind::null:
	default:
	    return std::string();
    }
}

// "a table" / "an apple" — the indefinite-article heuristic (initial
// vowel letter). Deliberately simple; a phrasing template on the entity
// overrides it where English is weirder than a heuristic.
inline std::string article(const std::string &noun)
{
    if ( noun.empty() )
	return noun;
    char c = noun[0];
    bool vowel = c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'
	      || c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U';
    return (vowel ? "an " : "a ") + noun;
}

// Count-picked form: plural(1,...) = "lamp", plural(3,...) = "lamps".
inline const std::string &plural(long n, const std::string &one,
				 const std::string &many)
{
    return n == 1 ? one : many;
}

// "a lamp, a key and an apple" — enumeration with a chosen conjunction
// (MUD style: no Oxford comma). Empty list -> empty string.
inline std::string enumerate(const std::vector<std::string> &items,
			     const std::string &conjunction = "and")
{
    std::string out;
    for ( size_t i = 0; i < items.size(); ++i )
    {
	if ( i > 0 )
	{
	    if ( i + 1 == items.size() )
		out += " " + conjunction + " ";
	    else
		out += ", ";
	}
	out += items[i];
    }
    return out;
}

// Sentence-case + terminal period for a composed fragment ("taken" ->
// "Taken."). Leaves existing terminal punctuation alone.
inline std::string sentence(const std::string &fragment)
{
    if ( fragment.empty() )
	return fragment;
    std::string out = fragment;
    if ( out[0] >= 'a' && out[0] <= 'z' )
	out[0] = (char)(out[0] - 'a' + 'A');
    char last = out[out.size() - 1];
    if ( last != '.' && last != '!' && last != '?' )
	out += '.';
    return out;
}

} // namespace prose
} // namespace hub
} // namespace madc

#endif // __MADCDIS_PROSE_H
