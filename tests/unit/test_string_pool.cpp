// Unit tests for string_pool.h: the arena-model interned string table (the
// backing store for TokenRec.spelling_id). Verifies dedup, distinctness, the
// str()/c_str() roundtrip, the reserved empty id, and growth/rehash integrity.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <cstdio>
#include <string>
#include <vector>
#include "string_pool.h"

TEST_CASE("intern dedups and assigns stable ids")
{
    StringPool p;
    uint32_t a = p.intern("vector");
    uint32_t b = p.intern("vector");   // same spelling -> same id
    uint32_t c = p.intern("map");
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != 0u);
    CHECK(p.str(a) == "vector");
    CHECK(p.str(c) == "map");
    CHECK(p.length(a) == 6u);
}

TEST_CASE("empty string is the reserved id 0")
{
    StringPool p;
    CHECK(p.intern("") == 0u);
    CHECK(p.intern(std::string()) == 0u);
}

TEST_CASE("embedded NUL bytes are honored by length, not strlen")
{
    StringPool p;
    uint32_t a = p.intern("ab\0cd", 5);
    uint32_t b = p.intern("ab\0ce", 5);
    CHECK(a != b);
    CHECK(p.length(a) == 5u);
    CHECK(p.intern("ab\0cd", 5) == a);   // dedup with embedded NUL
}

TEST_CASE("growth/rehash preserves ids and dedup across many entries")
{
    StringPool p;
    const int N = 5000;
    std::vector<uint32_t> ids;
    char buf[32];
    for (int i = 0; i < N; ++i) { snprintf(buf, sizeof buf, "id_%d", i); ids.push_back(p.intern(buf)); }
    for (int i = 0; i < N; ++i) { snprintf(buf, sizeof buf, "id_%d", i); CHECK(p.intern(buf) == ids[(size_t)i]); }
    for (int i = 1; i < N; ++i) CHECK(ids[(size_t)i] != ids[(size_t)i-1]);
    CHECK(p.count() == (size_t)N);
    CHECK(p.str(ids[0]) == "id_0");
    CHECK(p.str(ids[N-1]) == std::string("id_") + std::to_string(N-1));
}

TEST_CASE("reserve does not change ids")
{
    StringPool p;
    uint32_t a = p.intern("alpha");
    p.reserve(1u << 20, 1u << 16);
    CHECK(p.intern("alpha") == a);
    CHECK(p.str(a) == "alpha");
}
