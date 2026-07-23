// Unit tests for madc::dis::value_pool (madcdis/value_pool.h): the wide-value
// pool behind TokenInt::wide_handle. Verifies handle-0 reservation, dedup
// (same limbs -> same handle), lo64/hi64 round-trip, growth/rehash handle
// stability, and the serialization block accessors' consistency.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <vector>
#include "madcdis/value_pool.h"

using madc::dis::value_pool;

TEST_CASE("handle 0 is reserved; empty put returns it")
{
    value_pool p;
    CHECK(p.put((const uint64_t *)0, 0) == 0u);
    CHECK(p.count() == 0u);
}

TEST_CASE("put dedups identical limb sequences")
{
    value_pool p;
    uint32_t a = p.put_u128(0x1234, 0x5678);
    uint32_t b = p.put_u128(0x1234, 0x5678);
    uint32_t c = p.put_u128(0x1234, 0x5679);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != 0u);
    CHECK(p.count() == 2u);
    CHECK(p.lo64(a) == 0x1234u);
    CHECK(p.hi64(a) == 0x5678u);
    CHECK(p.hi64(c) == 0x5679u);
}

TEST_CASE("different limb counts with equal prefixes are distinct values")
{
    value_pool p;
    uint64_t two[2] = { 7, 0 };
    uint64_t three[3] = { 7, 0, 0 };
    uint32_t a = p.put(two, 2);
    uint32_t b = p.put(three, 3);
    CHECK(a != b);
    CHECK(p.nlimbs(a) == 2u);
    CHECK(p.nlimbs(b) == 3u);
}

TEST_CASE("growth/rehash preserves handles and dedup across many values")
{
    value_pool p;
    const int N = 3000;
    std::vector<uint32_t> hs;
    for ( int i = 0; i < N; ++i )
	hs.push_back(p.put_u128((uint64_t)i, (uint64_t)i * 31 + 1));
    for ( int i = 0; i < N; ++i )
	CHECK(p.put_u128((uint64_t)i, (uint64_t)i * 31 + 1) == hs[(size_t)i]);
    CHECK(p.count() == (size_t)N);
    for ( int i = 0; i < N; ++i )
    {
	CHECK(p.lo64(hs[(size_t)i]) == (uint64_t)i);
	CHECK(p.hi64(hs[(size_t)i]) == (uint64_t)i * 31 + 1);
    }
}

TEST_CASE("serialization blocks stay consistent with the live pool")
{
    value_pool p;
    uint32_t a = p.put_u128(0xdeadbeef, 0xfeedface);
    CHECK(p.entries_size() == p.count() + 1);          // + reserved handle 0
    CHECK(p.limbs_size() >= 2u);
    const value_pool::Entry *es = p.entries_data();
    CHECK(es[a].nlimbs == 2u);
    CHECK(p.limbs_data()[es[a].off] == 0xdeadbeefu);
    CHECK(p.limbs_data()[es[a].off + 1] == 0xfeedfaceu);
    CHECK((p.buckets_size() & (p.buckets_size() - 1)) == 0u);   // power of two
}
