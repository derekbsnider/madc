// Round-trip tests for the PUBLIC madc::dis substrate surface, reached ONLY
// through the curated umbrella <libmadc/dis.h> (never the internal madcdis/*.h
// paths) — exercising the export exactly as a C++ host embedding libmadc would
// (docs/plans/2026-07-06-madcdis-export-surface.md, step 2a). One round-trip
// per primitive: intern_table, id_table, value_pool, snapshot, pod_record.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <cstring>
#include <vector>
#include <string>

#include "libmadc/dis.h"

TEST_CASE("dis::intern_table — dedup + c_str round-trip")
{
    madc::dis::intern_table t;
    uint32_t foo = t.intern("foo");
    uint32_t bar = t.intern("bar");
    uint32_t foo2 = t.intern(std::string("foo"));
    CHECK(foo == foo2);				// same bytes -> same id
    CHECK(foo != bar);
    CHECK(foo != 0u);
    CHECK(t.count() == 2u);			// two distinct strings
    CHECK(std::string(t.c_str(foo)) == "foo");
    CHECK(t.length(bar) == 3u);
}

TEST_CASE("dis::id_table<T> — stable id <-> object, base offset honoured")
{
    int a = 10, b = 20, c = 30;
    madc::dis::id_table<int> tab(0x100u);	// a SYSTEM-segment-style base
    uint32_t ia = tab.add(&a);
    uint32_t ib = tab.add(&b);
    uint32_t ic = tab.add(&c);
    CHECK(ia == 0x100u);
    CHECK(ib == 0x101u);
    CHECK(ic == 0x102u);
    CHECK(tab.get(ia) == &a);
    CHECK(tab.get(ic) == &c);
    CHECK(tab.size() == 3u);
    CHECK(tab.get(0x0fffu) == (int *)0);	// below base
    CHECK(tab.get(0x103u) == (int *)0);		// past end
}

TEST_CASE("dis::value_pool — wide-value handle dedup + limb round-trip")
{
    madc::dis::value_pool p;
    uint32_t h1 = p.put_u128(0xdeadbeefu, 0x0123456789abcdefULL);
    uint32_t h2 = p.put_u128(0xdeadbeefu, 0x0123456789abcdefULL);
    uint32_t h3 = p.put_u128(0xdeadbeefu, 0x0123456789abce00ULL);
    CHECK(h1 == h2);				// identical limbs -> same handle
    CHECK(h1 != h3);
    CHECK(h1 != 0u);
    CHECK(p.count() == 2u);
    CHECK(p.lo64(h1) == 0xdeadbeefu);
    CHECK(p.hi64(h1) == 0x0123456789abcdefULL);
}

TEST_CASE("dis::snapshot — writer -> reader segment round-trip")
{
    madc::dis::snapshot_writer w;
    const char payload[] = "hello-substrate";
    const char other[]   = "second-segment";
    CHECK(w.add_segment(7u, 0u, payload, sizeof(payload), PchCompression::None));
    CHECK(w.add_segment(9u, 0u, other, sizeof(other), PchCompression::None));
    std::vector<uint8_t> blob;
    CHECK(w.build(blob));
    CHECK(blob.size() > 0u);

    madc::dis::snapshot_reader r;
    CHECK(r.open(blob.data(), blob.size()));
    CHECK(r.segment_count() == 2u);

    const madc::dis::snapshot_segment *seg = r.find(7u);
    REQUIRE(seg != (const madc::dis::snapshot_segment *)0);
    std::vector<uint8_t> got;
    CHECK(r.read_segment(*seg, got));
    CHECK(got.size() == sizeof(payload));
    CHECK(memcmp(got.data(), payload, sizeof(payload)) == 0);

    CHECK(r.find(123u) == (const madc::dis::snapshot_segment *)0);	// absent
}

TEST_CASE("dis::pod_record — append + bounds-checked read through the umbrella")
{
    struct Rec { uint32_t a; int32_t b; uint32_t c; };
    std::vector<uint32_t> buf;
    Rec in = { 5u, -6, 7u };
    uint32_t off = madc::dis::pod_append(buf, in);
    CHECK(off == 0u);
    CHECK(buf.size() == madc::dis::pod_words<Rec>());
    Rec out;
    CHECK(madc::dis::pod_read(buf, off, out));
    CHECK(out.a == 5u);
    CHECK(out.b == -6);
    CHECK(out.c == 7u);
    buf.pop_back();				// truncate -> read must fail
    CHECK_FALSE(madc::dis::pod_read(buf, 0, out));
}
