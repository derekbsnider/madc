#ifndef __MADCDIS_ARENA_H
#define __MADCDIS_ARENA_H 1

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

// madcdis/arena.h — the bump-allocator madc::dis substrate primitive.
//
// A variable-size BUMP allocator: each request gets exactly its size (16-aligned),
// from chunks that never relocate, with NO per-object free (drop-all is the unit
// of reclamation). Lifted from the allocator half of the compiler's TokenArena
// (docs/plans/2026-06-23-p1-token-arena-implementation-plan.md) and kept FREE of
// any domain coupling — the token slot registry that used to live here stays a
// token-specific consumer (TokenArena now HAS-A madc::dis::arena). See
// docs/plans/madcdis-plan.md + docs/plans/2026-06-29-madc-development-substrate-vision.md.
//
// Properties:
//   - VARIABLE size: each alloc() returns exactly sz bytes, 16-aligned.
//   - STABLE: chunks never relocate, so raw pointers handed out stay valid for the
//     life of the arena (the pointer-stability invariant the token stream needs).
//   - NO per-object free: an allocation is permanent until the arena is destroyed.
//
// THE INTERFACE (the contract arena variants in the catalog honor):
//   void  *alloc(size_t sz)   -> sz bytes, 16-aligned, from a never-relocated chunk
//   size_t bytes()            -> total bytes handed out (stat)
//   size_t count()            -> number of allocations (stat)
//   size_t chunks()           -> number of backing chunks (stat)
//
// THE CATALOG (variants slot in behind this interface; build from proven use):
//   * arena                   — bump, drop-all, heap-backed (THIS one)
//   * (future) slab/typed     — fixed-size record pool
//   * (future) size-class     — multi-size general allocator
//   * (future) page-refcounted, shm-backed, mmap-file

namespace madc {
namespace dis {

class arena
{
    struct Chunk { char *base; size_t used; size_t cap; };
    std::vector<Chunk> _chunks;
    size_t _default_cap;
    size_t _total;		// total bytes handed out (stat)
    size_t _count;		// number of allocations (stat)

    void add_chunk(size_t need)
    {
	size_t cap = need > _default_cap ? need : _default_cap;
	char *base = (char *)::malloc(cap);
	if ( !base )
	{
	    // Honor the process new-handler contract before giving up, the
	    // way operator new does: the installed handler may report context
	    // (e.g. which resource guard tripped) and/or free memory. If it
	    // returns, retry once.
	    std::new_handler h = std::get_new_handler();
	    if ( h )
	    {
		h();	// may throw; may free memory and return
		base = (char *)::malloc(cap);
	    }
	    if ( !base )
		throw std::bad_alloc();
	}
	Chunk c; c.base = base; c.used = 0; c.cap = cap;
	_chunks.push_back(c);
    }
public:
    explicit arena(size_t chunk_bytes = (size_t)1 << 20)
	: _default_cap(chunk_bytes), _total(0), _count(0) {}
    ~arena() { for ( size_t i = 0; i < _chunks.size(); ++i ) ::free(_chunks[i].base); }

    // bump-allocate sz bytes (16-aligned), from the current chunk or a fresh one.
    void *alloc(size_t sz)
    {
	sz = (sz + 15) & ~(size_t)15;
	if ( _chunks.empty() || _chunks.back().used + sz > _chunks.back().cap )
	    add_chunk(sz);
	Chunk &c = _chunks.back();
	void *p = c.base + c.used;
	c.used += sz;
	_total += sz;
	++_count;
	return p;
    }

    size_t bytes()  const { return _total; }
    size_t count()  const { return _count; }
    size_t chunks() const { return _chunks.size(); }
};

} // namespace dis
} // namespace madc

#endif // __MADCDIS_ARENA_H
