// Unit tests for madc::dis::pod_record (madcdis/pod_record.h): the fixed-stride
// POD (de)serialization primitive over a uint32 buffer. Verifies the stride
// (pod_words), append returning the start word offset + growing the buffer by
// exactly one record, byte-identical round-trip via pod_read, back-to-back
// packing read by index, appending into a non-empty buffer, and the bounds
// check (a truncated buffer reads false and leaves the out record untouched).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <vector>
#include <cstdint>
#include "madcdis/pod_record.h"

using madc::dis::pod_words;
using madc::dis::pod_append;
using madc::dis::pod_read;

namespace {
// A trivially-copyable record three uint32 words wide (mixed field types that
// still sum to a whole number of words), standing in for a cir_forest_type_*.
struct Rec { uint32_t a; int32_t b; uint32_t c; };
}

TEST_CASE("pod_words is the record's width in uint32 words")
{
    CHECK(pod_words<Rec>() == 3u);
    CHECK(pod_words<uint32_t>() == 1u);
    CHECK(pod_words<uint64_t>() == 2u);
}

TEST_CASE("append returns the start offset and grows the buffer by one stride")
{
    std::vector<uint32_t> buf;
    Rec r = { 10u, -7, 0xdeadbeefu };
    uint32_t off = pod_append(buf, r);
    CHECK(off == 0u);
    CHECK(buf.size() == pod_words<Rec>());
}

TEST_CASE("read round-trips a record byte-identically")
{
    std::vector<uint32_t> buf;
    Rec in = { 42u, -100, 0x12345678u };
    uint32_t off = pod_append(buf, in);
    Rec out;
    CHECK(pod_read(buf, off, out));
    CHECK(out.a == in.a);
    CHECK(out.b == in.b);
    CHECK(out.c == in.c);
}

TEST_CASE("records pack back-to-back and read by index at begin + i*stride")
{
    std::vector<uint32_t> buf;
    const uint32_t begin = (uint32_t)buf.size();
    Rec recs[3] = { {1u, -1, 100u}, {2u, -2, 200u}, {3u, -3, 300u} };
    for (int i = 0; i < 3; ++i)
	CHECK(pod_append(buf, recs[i]) == begin + (uint32_t)i * pod_words<Rec>());
    CHECK(buf.size() == 3u * pod_words<Rec>());
    for (int i = 0; i < 3; ++i) {
	Rec out;
	CHECK(pod_read(buf, begin + (size_t)i * pod_words<Rec>(), out));
	CHECK(out.a == recs[i].a);
	CHECK(out.b == recs[i].b);
	CHECK(out.c == recs[i].c);
    }
}

TEST_CASE("append into a non-empty buffer returns the prior size as offset")
{
    std::vector<uint32_t> buf(5u, 0xffffffffu);	// 5 words of unrelated data
    Rec r = { 7u, 8, 9u };
    uint32_t off = pod_append(buf, r);
    CHECK(off == 5u);
    Rec out;
    CHECK(pod_read(buf, off, out));
    CHECK(out.a == 7u);
    CHECK(out.c == 9u);
}

TEST_CASE("read past the end fails and leaves the out record untouched")
{
    std::vector<uint32_t> buf;
    Rec r = { 1u, 2, 3u };
    pod_append(buf, r);
    buf.pop_back();				// truncate: one word short of a full Rec
    Rec out = { 111u, 222, 333u };
    CHECK_FALSE(pod_read(buf, 0, out));
    CHECK(out.a == 111u);			// untouched
    CHECK(out.c == 333u);
    // Off exactly at end (empty tail) is also OOB for a non-empty record.
    CHECK_FALSE(pod_read(buf, buf.size(), out));
}
