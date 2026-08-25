// Unit battery for madcdis/doc_lens.h — the document-lens coordinate map
// (madcide AST-3, the view seam). Pins the projection CONTRACT: 1:1 inside
// copy segments (caret ends included), forward collapse strictly inside
// gaps, just-after-the-last-image past the end, 0 for the empty map — and
// the strict value codec (a map is a contract; malformed rows refuse the
// whole map). Design: docs/plans/2026-08-25-madcide-ast-arc-design.md §2.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "madcdis/doc_lens.h"

using madc::hub::doc_map;

TEST_CASE("identity map — display == stored at every caret position")
{
    doc_map m;
    CHECK(m.add(0, 0, 10));
    for ( size_t i = 0; i <= 10; ++i )
    {
	CHECK(m.to_display(i) == i);
	CHECK(m.to_stored(i) == i);
    }
    // Past the end: just after the last image.
    CHECK(m.to_display(11) == 10u);
    CHECK(m.to_stored(99) == 10u);
}

TEST_CASE("concealed ranges — the markdown caret math")
{
    // stored "ab**cd**e" displayed "abcde": the ** pairs are concealed.
    //   seg0 disp 0 stored 0 len 2   ("ab")
    //   seg1 disp 2 stored 4 len 2   ("cd"; stored 2..4 concealed)
    //   seg2 disp 4 stored 8 len 1   ("e";  stored 6..8 concealed)
    doc_map m;
    CHECK(m.add(0, 0, 2));
    CHECK(m.add(2, 4, 2));
    CHECK(m.add(4, 8, 1));

    // Inside copies: 1:1, segment ends included.
    CHECK(m.to_display(0) == 0u);
    CHECK(m.to_display(1) == 1u);
    CHECK(m.to_display(2) == 2u);	// seg0 end == the conceal boundary
    CHECK(m.to_display(4) == 2u);	// seg1 start
    CHECK(m.to_display(5) == 3u);
    CHECK(m.to_display(6) == 4u);	// seg1 end
    CHECK(m.to_display(8) == 4u);	// seg2 start
    CHECK(m.to_display(9) == 5u);	// seg2 end (document end)

    // STRICTLY inside a concealed range: forward collapse.
    CHECK(m.to_display(3) == 2u);
    CHECK(m.to_display(7) == 4u);

    // Past everything: just after the last image.
    CHECK(m.to_display(12) == 5u);

    // The inverse: display carets land on stored positions.
    CHECK(m.to_stored(0) == 0u);
    CHECK(m.to_stored(2) == 2u);	// boundary: the copy's own end wins
    CHECK(m.to_stored(3) == 5u);
    CHECK(m.to_stored(4) == 6u);
    CHECK(m.to_stored(5) == 9u);
    CHECK(m.to_stored(6) == 9u);	// past the end

    // Round trip: exact for every stored caret that is not gap-shadowed
    // (a gap-START caret shares its display position with the preceding
    // copy's end and collapses to that EARLIER position — outside the
    // concealed run, the safe side for a future put)...
    size_t exact[] = { 0, 1, 2, 5, 6, 9 };
    for ( size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); ++i )
	CHECK(m.to_stored(m.to_display(exact[i])) == exact[i]);
    // ...a documented, deterministic collapse for gap-adjacent carets...
    CHECK(m.to_stored(m.to_display(3)) == 2u);	// inside the conceal
    CHECK(m.to_stored(m.to_display(4)) == 2u);	// at its far edge
    CHECK(m.to_stored(m.to_display(7)) == 6u);
    CHECK(m.to_stored(m.to_display(8)) == 6u);
    // ...and EVERY display caret round-trips exactly (display space is
    // gap-free here, so it carries no ambiguity).
    for ( size_t d = 0; d <= 5; ++d )
	CHECK(m.to_display(m.to_stored(d)) == d);
}

TEST_CASE("synthetic ranges — display decoration with no stored home")
{
    // stored "abcde" displayed "abc>>de": display 3..5 is synthetic.
    //   seg0 disp 0 stored 0 len 3
    //   seg1 disp 5 stored 3 len 2
    doc_map m;
    CHECK(m.add(0, 0, 3));
    CHECK(m.add(5, 3, 2));

    CHECK(m.to_stored(3) == 3u);	// seg0 end: before the decoration
    CHECK(m.to_stored(4) == 3u);	// strictly inside: forward collapse
    CHECK(m.to_stored(5) == 3u);	// seg1 start
    CHECK(m.to_stored(6) == 4u);
    CHECK(m.to_stored(7) == 5u);
    CHECK(m.to_stored(8) == 5u);	// past the end

    CHECK(m.to_display(3) == 3u);	// the stored caret stays BEFORE
    CHECK(m.to_display(4) == 6u);	// the synthetic run
    CHECK(m.to_display(5) == 7u);
}

TEST_CASE("empty map — a wholly rendered view: everything parks at 0")
{
    doc_map m;
    CHECK(m.empty());
    CHECK(m.to_display(0) == 0u);
    CHECK(m.to_display(42) == 0u);
    CHECK(m.to_stored(0) == 0u);
    CHECK(m.to_stored(42) == 0u);
}

TEST_CASE("add — monotone in BOTH axes, non-empty; refusals change nothing")
{
    doc_map m;
    CHECK(!m.add(0, 0, 0));		// empty segment
    CHECK(m.add(0, 0, 4));
    CHECK(!m.add(3, 4, 2));		// display overlap
    CHECK(!m.add(4, 3, 2));		// stored overlap
    CHECK(m.seg_count() == 1u);
    CHECK(m.add(4, 4, 2));		// contiguous is fine
    CHECK(m.add(8, 10, 1));		// gaps on both axes are fine
    CHECK(m.seg_count() == 3u);
}

TEST_CASE("value codec — round trip; a malformed row refuses the map")
{
    doc_map m;
    CHECK(m.add(0, 0, 2));
    CHECK(m.add(2, 4, 2));
    madc::value v = m.to_value();
    CHECK(v.is_array());
    CHECK(v.as_array().size() == 2u);

    doc_map back;
    CHECK(doc_map::from_value(v, back));
    CHECK(back.seg_count() == 2u);
    for ( size_t i = 0; i <= 6; ++i )
	CHECK(back.to_display(i) == m.to_display(i));

    // The empty array is the empty map (a rendered view's honest answer).
    doc_map none;
    CHECK(doc_map::from_value(madc::value::make_array(
	std::vector<madc::value>()), none));
    CHECK(none.empty());

    // Refusals: non-array, missing field, negative field, non-monotone.
    doc_map r;
    CHECK(!doc_map::from_value(madc::value((int64_t)7), r));
    std::map<std::string, madc::value> bad;
    bad["disp"] = madc::value((int64_t)0);
    bad["stored"] = madc::value((int64_t)0);
    std::vector<madc::value> rows;
    rows.push_back(madc::value::make_object(bad));	// no "len"
    CHECK(!doc_map::from_value(madc::value::make_array(rows), r));
    CHECK(r.empty());
    bad["len"] = madc::value((int64_t)-1);
    rows.clear();
    rows.push_back(madc::value::make_object(bad));
    CHECK(!doc_map::from_value(madc::value::make_array(rows), r));
    bad["len"] = madc::value((int64_t)4);
    std::map<std::string, madc::value> bad2(bad);	// overlaps bad
    rows.clear();
    rows.push_back(madc::value::make_object(bad));
    rows.push_back(madc::value::make_object(bad2));
    CHECK(!doc_map::from_value(madc::value::make_array(rows), r));
    CHECK(r.empty());
}
