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
#include <vector>
#include "madcdis/arena.h"	// the generic bump-allocator primitive (madc::dis::arena)

class TokenBase;	// slot registry maps stable uint32 ids <-> token pointers

// TokenArena is the TOKEN-SPECIFIC consumer of the generic madc::dis::arena
// primitive: it HAS-A bump allocator (for TokenBase storage, via
// TokenBase::operator new) and adds the token slot registry on top. The
// allocator itself carries no TokenBase coupling — that lives here, where it
// belongs (the substrate primitive stays general; the compiler's id<->pointer
// bridge is a consumer). See docs/plans/2026-06-29-madc-development-substrate-vision.md.
class TokenArena
{
    madc::dis::arena _arena;	// the generic bump allocator (drop-all, stable pointers)
    // Slot registry: stable uint32 slot-id -> TokenBase*. A token is registered
    // lazily (first time it is referenced by id — slot_id_for), so the registry
    // holds only id-referenced tokens. _slots[0] is a reserved NULL sentinel
    // (id 0 == "no token"). Pointers index into the arena's never-relocated
    // chunks, so they stay valid for the arena's life. The flat id-vectors that
    // replace vector<TokenBase*> (children, macro/template bodies) hold these ids.
    std::vector<TokenBase *> _slots;
public:
    explicit TokenArena(size_t chunk_bytes = (size_t)1 << 20) : _arena(chunk_bytes) {}

    // Token allocation + the --show-stats trio delegate to the generic arena.
    void  *alloc(size_t sz) { return _arena.alloc(sz); }
    size_t bytes()  const { return _arena.bytes(); }
    size_t count()  const { return _arena.count(); }
    size_t chunks() const { return _arena.chunks(); }

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
