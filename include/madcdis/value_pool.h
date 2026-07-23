#ifndef __MADCDIS_VALUE_POOL_H
#define __MADCDIS_VALUE_POOL_H 1

#include <cstdint>
#include <cstring>
#include <vector>

#include "madcdis/intern_table.h"	// hash_init/hash_step (the one hash discipline)

// madcdis/value_pool.h — the WIDE-VALUE pool madc::dis substrate primitive.
//
// Stores integer values WIDER than 64 bits as (nlimbs, uint64 limbs) records
// behind a stable uint32 handle. Values that fit 64 bits stay INLINE in their
// consumers (the fast path) and never enter the pool. Identical limb sequences
// dedup to the same handle (intern discipline), so a handle compare IS a value
// compare. Handle 0 is reserved as "none".
//
// This is P0 slice 2 of docs/plans/2026-06-12-p0-value-pool-plan.md: the
// handle is the reference shape the flat token record and the serialized
// cir_node literal carry (type_id + value-handle) — consumed by the forest
// (Track B of docs/plans/2026-07-04-data-substrate-first-customer-PLAN.md).
// Width is per-entry (limb count), so _BitInt(N) values slot in later without
// a format change; today's sole producer stores 2-limb (128-bit) values.
//
// Same block discipline as intern_table: THREE index-linked blocks (limb
// block, entry block, bucket block) that serialize / rebind with zero fixup.
//
// THE INTERFACE:
//   uint32_t put(limbs, nlimbs)  -> stable handle (dedups)
//   const uint64_t *limbs(h)     -> the limb data (little-endian limb order)
//   uint32_t nlimbs(h)           -> limb count
//   put_u128/lo64/hi64           -> 2-limb conveniences (the current width)
//   size_t count()               -> live entries (excl. handle 0)

namespace madc {
namespace dis {

class value_pool
{
public:
    // One record; also the on-disk entry-block shape (16 bytes, fixed layout).
    struct Entry { uint32_t off; uint32_t nlimbs; uint32_t hash; uint32_t next; };
private:
    mutable std::vector<uint64_t> _limbs;    // limb data, append-only
    mutable std::vector<Entry>    _entries;  // handle == index; handle 0 = none
    mutable std::vector<uint32_t> _buckets;  // power-of-two; head entry-index or NIL
    enum : uint32_t { NIL = 0xffffffffu };

    void rehash(size_t nbuckets) const
    {
	_buckets.assign(nbuckets, NIL);
	uint32_t mask = (uint32_t)nbuckets - 1;
	for ( uint32_t i = 1; i < _entries.size(); ++i )
	{
	    uint32_t b = _entries[i].hash & mask;
	    _entries[i].next = _buckets[b];
	    _buckets[b] = i;
	}
    }
    static uint32_t hash_limbs(const uint64_t *limbs, uint32_t nlimbs)
    {
	uint32_t h = intern_table::hash_init();
	const unsigned char *p = (const unsigned char *)limbs;
	for ( uint32_t i = 0; i < nlimbs * 8; ++i )
	    h = intern_table::hash_step(h, p[i]);
	return h;
    }
public:
    enum : uint32_t { none = 0 };

    value_pool()
    {
	Entry e0 = { 0, 0, 0, NIL };
	_entries.push_back(e0);          // handle 0 = none
	_buckets.assign(256, NIL);
    }

    // Intern a limb sequence -> stable handle (dedups). nlimbs 0 -> handle 0.
    uint32_t put(const uint64_t *limbs, uint32_t nlimbs) const
    {
	if ( nlimbs == 0 ) return none;
	uint32_t h = hash_limbs(limbs, nlimbs);
	uint32_t mask = (uint32_t)_buckets.size() - 1;
	uint32_t b = h & mask;
	for ( uint32_t i = _buckets[b]; i != NIL; i = _entries[i].next )
	{
	    const Entry &e = _entries[i];
	    if ( e.hash == h && e.nlimbs == nlimbs
		 && memcmp(&_limbs[e.off], limbs, (size_t)nlimbs * 8) == 0 )
		return i;                // dedup hit
	}
	if ( _entries.size() > _buckets.size() )   // grow at load factor 1
	{
	    rehash(_buckets.size() * 2);
	    mask = (uint32_t)_buckets.size() - 1;
	    b = h & mask;
	}
	uint32_t off = (uint32_t)_limbs.size();
	_limbs.insert(_limbs.end(), limbs, limbs + nlimbs);
	uint32_t handle = (uint32_t)_entries.size();
	Entry e = { off, nlimbs, h, _buckets[b] };
	_entries.push_back(e);
	_buckets[b] = handle;
	return handle;
    }

    const uint64_t *limbs(uint32_t h) const  { return &_limbs[_entries[h].off]; }
    uint32_t        nlimbs(uint32_t h) const { return _entries[h].nlimbs; }
    size_t          count() const            { return _entries.size() - 1; }

    // 2-limb (128-bit) conveniences — the current sole producer width.
    uint32_t put_u128(uint64_t lo, uint64_t hi) const
    {
	uint64_t l[2] = { lo, hi };
	return put(l, 2);
    }
    uint64_t lo64(uint32_t h) const { return _entries[h].nlimbs >= 1 ? limbs(h)[0] : 0; }
    uint64_t hi64(uint32_t h) const { return _entries[h].nlimbs >= 2 ? limbs(h)[1] : 0; }

    // --- serialization accessors (the three blocks; same contract as
    // intern_table — index-linked, zero fixup). Sizes in ELEMENTS.
    const uint64_t *limbs_data()   const { return _limbs.data(); }
    size_t          limbs_size()   const { return _limbs.size(); }
    const Entry    *entries_data() const { return _entries.data(); }
    size_t          entries_size() const { return _entries.size(); }
    const uint32_t *buckets_data() const { return _buckets.data(); }
    size_t          buckets_size() const { return _buckets.size(); }
};

} // namespace dis
} // namespace madc

#endif // __MADCDIS_VALUE_POOL_H
