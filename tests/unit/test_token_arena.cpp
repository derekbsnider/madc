// Unit tests for token_arena.h: the variable-size bump allocator + slot
// registry that backs every TokenBase allocation (Phase 1) and the stable
// slot-id <-> token bridge (Phase 2, children->slot-ids). Tests the arena
// mechanism directly with fabricated token pointers — the registry only
// stores/returns pointers, so it needs no real TokenBase construction.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <cstdint>
#include <vector>
#include "token_arena.h"

// stand-in token addresses (the registry stores opaque TokenBase*). TokenBase
// is incomplete here, so compare slots as void* — doctest's expression
// decomposition can't form references over an incomplete pointee type.
static TokenBase *fake(uintptr_t n) { return reinterpret_cast<TokenBase *>(n); }
static const void *vp(TokenBase *t) { return reinterpret_cast<const void *>(t); }

TEST_CASE("bump alloc is 16-aligned and never returns the same address twice")
{
    TokenArena a;
    void *p = a.alloc(1);
    void *q = a.alloc(152);
    void *r = a.alloc(1);
    CHECK((reinterpret_cast<uintptr_t>(p) & 15u) == 0u);
    CHECK((reinterpret_cast<uintptr_t>(q) & 15u) == 0u);
    CHECK(p != q);
    CHECK(q != r);
    CHECK(a.count() == 3u);
    CHECK(a.bytes() == 16u + 160u + 16u);   // sizes rounded up to 16
}

TEST_CASE("oversize allocation crosses into a fresh chunk, stays valid")
{
    TokenArena a(64);                 // tiny chunk to force growth
    void *small = a.alloc(32);
    void *big = a.alloc(4096);        // larger than the default chunk
    void *after = a.alloc(32);
    CHECK(small != NULL);
    CHECK(big != NULL);
    CHECK(after != NULL);
    CHECK(a.chunks() >= 2u);
}

TEST_CASE("slot registry: id 0 is the reserved NULL sentinel")
{
    TokenArena a;
    CHECK(vp(a.slot(0)) == (const void *)NULL);   // nothing registered yet
    uint32_t id = a.register_slot(fake(0x1000));
    CHECK(id == 1u);                  // first real id is 1, [0] is the sentinel
    CHECK(vp(a.slot(0)) == (const void *)NULL);   // sentinel still NULL after
}

TEST_CASE("slot registry round-trips id <-> pointer and grows monotonically")
{
    TokenArena a;
    TokenBase *t1 = fake(0x1000), *t2 = fake(0x2000), *t3 = fake(0x3000);
    uint32_t i1 = a.register_slot(t1);
    uint32_t i2 = a.register_slot(t2);
    uint32_t i3 = a.register_slot(t3);
    CHECK(i1 == 1u);
    CHECK(i2 == 2u);
    CHECK(i3 == 3u);
    CHECK(vp(a.slot(i1)) == vp(t1));
    CHECK(vp(a.slot(i2)) == vp(t2));
    CHECK(vp(a.slot(i3)) == vp(t3));
    CHECK(a.slot_count() == 4u);      // [0] sentinel + 3 tokens
    CHECK(vp(a.slot(99u)) == (const void *)NULL); // out of range
}
