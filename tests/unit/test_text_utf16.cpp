#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "madcdis/text_utf16.h"

#include <string>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

using madc::col16_of_byte;
using madc::col16_to_byte;

TEST_CASE("ASCII: a UTF-16 column IS the byte column")
{
	const std::string t = "long add(long a, long b)";
	for ( std::size_t i = 0; i <= t.size(); ++i )
	{
		CHECK(col16_to_byte(t, (long)i) == i);
		CHECK(col16_of_byte(t, i) == (long)i);
	}
}

TEST_CASE("a two-byte code point is one UTF-16 unit")
{
	// "café" — 'é' is U+00E9, two bytes, one UTF-16 unit.
	const std::string t = "caf\xC3\xA9x";
	CHECK(t.size() == 6);
	CHECK(col16_to_byte(t, 3) == 3);	// before the é
	CHECK(col16_to_byte(t, 4) == 5);	// after it: two bytes consumed
	CHECK(col16_to_byte(t, 5) == 6);	// after the x
	CHECK(col16_of_byte(t, 3) == 3);
	CHECK(col16_of_byte(t, 5) == 4);
	CHECK(col16_of_byte(t, 6) == 5);
}

TEST_CASE("a three-byte code point is one UTF-16 unit")
{
	// "a→b" — U+2192 is three bytes, one UTF-16 unit.
	const std::string t = "a\xE2\x86\x92" "b";
	CHECK(t.size() == 5);
	CHECK(col16_to_byte(t, 1) == 1);
	CHECK(col16_to_byte(t, 2) == 4);
	CHECK(col16_to_byte(t, 3) == 5);
	CHECK(col16_of_byte(t, 4) == 2);
	CHECK(col16_of_byte(t, 5) == 3);
}

TEST_CASE("a four-byte code point is a surrogate PAIR — two units")
{
	// "a😀b" — U+1F600 is four bytes and two UTF-16 units.
	const std::string t = "a\xF0\x9F\x98\x80" "b";
	CHECK(t.size() == 6);
	CHECK(col16_to_byte(t, 1) == 1);	// before the emoji
	CHECK(col16_to_byte(t, 3) == 5);	// past both units
	CHECK(col16_to_byte(t, 4) == 6);	// past the b
	CHECK(col16_of_byte(t, 1) == 1);
	CHECK(col16_of_byte(t, 5) == 3);
	CHECK(col16_of_byte(t, 6) == 4);
}

TEST_CASE("a column inside a surrogate pair snaps to the pair's start")
{
	const std::string t = "a\xF0\x9F\x98\x80" "b";
	CHECK(col16_to_byte(t, 2) == 1);	// half a code point is not a position
}

TEST_CASE("columns past the end clamp, negatives read as zero")
{
	const std::string t = "abc";
	CHECK(col16_to_byte(t, 99) == 3);
	CHECK(col16_to_byte(t, 0) == 0);
	CHECK(col16_to_byte(t, -4) == 0);
	CHECK(col16_of_byte(t, 99) == 3);
	CHECK(col16_of_byte(t, 0) == 0);
}

TEST_CASE("the two directions are inverses on a mixed line")
{
	// ASCII + 2-byte + 3-byte + 4-byte, interleaved.
	const std::string t = "id\xC3\xA9" "x\xE2\x86\x92" "y\xF0\x9F\x98\x80" "z";
	for ( long c = 0; c <= col16_of_byte(t, t.size()); ++c )
	{
		const std::size_t b = col16_to_byte(t, c);
		// Round-tripping a column that names a real boundary is exact;
		// a column inside a pair snaps back to the pair's start.
		CHECK(col16_of_byte(t, b) <= c);
		CHECK(col16_to_byte(t, col16_of_byte(t, b)) == b);
	}
}

TEST_CASE("an empty line has exactly one position")
{
	const std::string t;
	CHECK(col16_to_byte(t, 0) == 0);
	CHECK(col16_to_byte(t, 7) == 0);
	CHECK(col16_of_byte(t, 0) == 0);
}
