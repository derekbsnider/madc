#ifndef __MADCDIS_ID_TABLE_H
#define __MADCDIS_ID_TABLE_H 1

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <set>
#include <utility>
#include <vector>

// madcdis/id_table.h — the segmented stable-id table madc::dis substrate primitive.
//
// id_table<T> is a stable uint32-id <-> object* registry for ONE id segment: it
// models the "growing tail over a fixed base" half of a segmented id space.
// Objects are appended; each gets a stable id == base + index that reverse-maps
// to its pointer for the life of the table. There is NO removal — an id is
// permanent once assigned (drop-all on destruction, like madc::dis::arena).
//
// Storage is std::vector<T *>, NOT std::vector<T>: T may be polymorphic and
// growth must never invalidate a handed-out pointer (the same pointer-stability
// invariant the arena and token slot registry hold). The table stores raw,
// non-owning pointers — lifetime of the T objects belongs to the consumer.
//
// This is the segment primitive behind the compiler's type table
// (Program::type_id_for / type_from_id; design
// docs/plans/2026-06-12-type-table-value-abi-design.md §2). The split mirrors
// slice 3 (arena vs TokenArena): the primitive owns ONE segment's STORAGE; the
// consumer owns id POLICY — its own per-object memo field, the frozen primitive
// prefix, and the segment-base dispatch across primitive/system/project ranges.
// The value ABI's refcounted cell pools and madc::dat serialization instantiate
// id_table over their own segment bases (stable integer ids are exactly what
// position-independent mem://shm:// pools and serialized type-refs need, where a
// raw DataDef* cannot live). See
// docs/plans/2026-06-29-madc-development-substrate-vision.md.
//
// THE INTERFACE (the contract id_table variants in the catalog honor):
//   uint32_t add(T *obj)       -> assigns base + size(), appends, returns the id
//   T       *get(uint32_t id)  -> object for id, or NULL if id < base / past end
//   bool     set(uint32_t id, T *obj) -> repoint an ASSIGNED id to a new object
//                                 (one identity, a replacement object — e.g. the
//                                 struct->class promotion); false if id not ours
//   begin/commit/rollback_transaction -> first-write tracking for assigned slots
//                                 plus truncation of objects appended in scope
//   uint32_t base()            -> the segment base this table assigns ids from
//   size_t   size()            -> number of objects registered
//
// THE CATALOG (variants slot in behind this interface; build from proven use):
//   * id_table                 — append-only, in-memory, vector-backed (THIS one)
//   * (future) frozen/immutable — a read-only mmap'd segment (system forest)
//   * (future) memoized-derived — (kind, operand-id) -> derived-type id memo

namespace madc {
namespace dis {

template<class T>
class id_table
{
public:
    struct transaction_state
    {
	struct SavedSlot
	{
	    size_t index;
	    T *value;
	    SavedSlot(size_t i, T *v) : index(i), value(v) {}
	};
	size_t original_size;
	std::vector<SavedSlot> saved;
	std::set<size_t> touched;
	transaction_state() : original_size(0) {}
    };

private:
    std::vector<T *> _objs;
    uint32_t _base;
    transaction_state *_transaction;
public:
    explicit id_table(uint32_t base = 0)
	: _base(base), _transaction((transaction_state *)0) {}
    id_table(const id_table &other)
	: _objs(other._objs), _base(other._base),
	  _transaction((transaction_state *)0) {}
    id_table &operator=(const id_table &other)
    {
	if ( this != &other )
	{
	    assert(!_transaction);
	    _objs = other._objs;
	    _base = other._base;
	}
	return *this;
    }

    // Append obj and return its stable id (base + prior size). Idempotency is
    // the consumer's job (it holds the per-object memo); this always appends.
    uint32_t add(T *obj)
    {
	_objs.push_back(obj);
	return _base + (uint32_t)(_objs.size() - 1);
    }

    // Reverse lookup. NULL for an id below this segment's base or past its end
    // (an id from a foreign table / never registered here).
    T *get(uint32_t id) const
    {
	if ( id < _base )
	    return (T *)0;
	uint32_t idx = id - _base;
	return idx < _objs.size() ? _objs[idx] : (T *)0;
    }

    // Transaction diagnostics/rollback support: inspect an assigned slot by
    // segment-relative index without changing the stable public id policy.
    T *at_index(size_t index) const
    {
	return index < _objs.size() ? _objs[index] : (T *)0;
    }

    // Repoint an already-assigned id to a replacement object — ONE identity, a
    // new object (the struct-promoted-to-class case: the promotion copies the
    // type_id onto the fresh DataDefCLASS, so the id must resolve to it, not to
    // the superseded struct). Ids are still never removed or renumbered. False
    // for an id this table never assigned.
    bool set(uint32_t id, T *obj)
    {
	if ( id < _base )
	    return false;
	uint32_t idx = id - _base;
	if ( idx >= _objs.size() )
	    return false;
	if ( _transaction )
	{
	    std::pair<typename std::set<size_t>::iterator, bool> inserted =
		_transaction->touched.insert(idx);
	    if ( inserted.second )
		try
		{
		    _transaction->saved.push_back(
			typename transaction_state::SavedSlot(idx, _objs[idx]));
		}
		catch ( ... )
		{
		    _transaction->touched.erase(inserted.first);
		    throw;
		}
	}
	_objs[idx] = obj;
	return true;
    }

    void begin_transaction(transaction_state &state)
    {
	assert(!_transaction);
	state.original_size = _objs.size();
	state.saved.clear();
	state.touched.clear();
	_transaction = &state;
    }

    void commit_transaction(transaction_state &state)
    {
	assert(_transaction == &state);
	_transaction = (transaction_state *)0;
	state.original_size = 0;
	state.saved.clear();
	state.touched.clear();
    }

    void rollback_transaction(transaction_state &state)
    {
	assert(_transaction == &state);
	_transaction = (transaction_state *)0;
	for ( size_t i = state.saved.size(); i-- > 0; )
	{
	    const typename transaction_state::SavedSlot &saved = state.saved[i];
	    _objs[saved.index] = saved.value;
	}
	_objs.resize(state.original_size);
	state.original_size = 0;
	state.saved.clear();
	state.touched.clear();
    }

    uint32_t base() const { return _base; }
    size_t   size() const { return _objs.size(); }
};

} // namespace dis
} // namespace madc

#endif // __MADCDIS_ID_TABLE_H
