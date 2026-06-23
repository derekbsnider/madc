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

class TokenBase;	// slot registry maps stable uint32 ids <-> token pointers

class TokenArena
{
    struct Chunk { char *base; size_t used; size_t cap; };
    std::vector<Chunk> _chunks;
    // Slot registry: stable uint32 slot-id -> TokenBase*. A token is registered
    // lazily (first time it is referenced by id — slot_id_for), so the registry
    // holds only id-referenced tokens. _slots[0] is a reserved NULL sentinel
    // (id 0 == "no token"). Pointers index into never-relocated chunks, so they
    // stay valid for the arena's life. The flat id-vectors that replace
    // vector<TokenBase*> (children, macro/template bodies) hold these ids.
    std::vector<TokenBase *> _slots;
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

    // Slot registry. register_slot assigns the next stable id to t (the [0]
    // sentinel is reserved on first use). slot() maps an id back to its token
    // (NULL for id 0 or out of range). The token caches its id in rec.slot_id;
    // callers should use madc_slot_id_for() (parser.cpp) for lazy idempotent
    // stamping rather than calling register_slot twice for the same token.
    uint32_t register_slot(TokenBase *t)
    {
	if ( _slots.empty() )
	    _slots.push_back((TokenBase *)0);	// reserve id 0 = "no token"
	_slots.push_back(t);
	return (uint32_t)(_slots.size() - 1);
    }
    TokenBase *slot(uint32_t id) const
    {
	return id < _slots.size() ? _slots[id] : (TokenBase *)0;
    }
    size_t slot_count() const { return _slots.size(); }
};

// Lazy idempotent slot-id stamp + reverse lookup over the per-process token
// arena (defined in parser.cpp alongside TokenBase::operator new). slot_id_for
// returns t's stable id (assigning one on first call); token_for_slot reverses
// it. id 0 == no token. These are the id<->pointer bridge for the flat
// id-vectors that replace vector<TokenBase*>.
uint32_t   madc_slot_id_for(TokenBase *t);
TokenBase *madc_token_for_slot(uint32_t id);

#endif // __TOKEN_ARENA_H
