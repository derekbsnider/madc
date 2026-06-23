#ifndef __TOKEN_ARENA_H
//////////////////////////////////////////////////////////////////////////
//									//
// madc Token Arena							//
//									//
// Phase 1 of the token-arena plan					//
// (docs/plans/2026-06-23-p1-token-arena-implementation-plan.md).	//
//									//
// A variable-size BUMP allocator backing every TokenBase allocation	//
// (routed via TokenBase::operator new). Properties:			//
//   - VARIABLE size: each token gets exactly sizeof(T) (16-aligned).	//
//     Uniform cells were only needed for a freelist; tokens are never	//
//     individually freed, so variable bump is denser and simpler.	//
//   - STABLE: chunks never relocate, so the raw TokenBase* held by	//
//     cir_node / TokenBase::parent / the many vector<TokenBase*> stay	//
//     valid for the life of the arena (the hard pointer-stability	//
//     invariant).							//
//   - NO per-token free: a token is permanent once allocated. The few	//
//     `delete tok` sites still run the virtual destructor (freeing that	//
//     token's std::string members) but the cell stays in the arena.	//
//									//
// Lifecycle (Phase 1): process-lifetime. Today nothing frees the token	//
// stream (no ~Program/~TokenStream deletes _buf), so a non-resetting	//
// arena is zero memory regression. There is deliberately NO reset()	//
// yet: bulk-freeing chunks while cells still hold std::string members	//
// (leading_trivia, TokenIdent::str) would leak those heap buffers. A	//
// safe reset() arrives in Phase 2, once the fields are flattened into	//
// a POD TokenRec (spelling_id, etc.) and cells are trivially		//
// destructible.							//
//////////////////////////////////////////////////////////////////////////
#define __TOKEN_ARENA_H 1

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

class TokenArena
{
    struct Chunk { char *base; size_t used; size_t cap; };
    std::vector<Chunk> _chunks;
    size_t _default_cap;
    size_t _total;		// total bytes handed out (stat / --show-stats)
    size_t _count;		// number of tokens allocated (stat)

    void add_chunk(size_t need)
    {
	size_t cap = need > _default_cap ? need : _default_cap;
	char *base = (char *)::malloc(cap);
	if ( !base )
	    throw std::bad_alloc();
	Chunk c; c.base = base; c.used = 0; c.cap = cap;
	_chunks.push_back(c);
    }
public:
    explicit TokenArena(size_t chunk_bytes = (size_t)1 << 20)
	: _default_cap(chunk_bytes), _total(0), _count(0) {}
    ~TokenArena() { for ( size_t i = 0; i < _chunks.size(); ++i ) ::free(_chunks[i].base); }

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

#endif // __TOKEN_ARENA_H
