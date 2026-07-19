#ifndef __MADCDIS_INTERN_TABLE_H
#define __MADCDIS_INTERN_TABLE_H 1

#include <cstdint>
#include <cassert>
#include <cstring>
#include <vector>
#include <string>

// madcdis/intern_table.h — the FIRST madc::dis substrate primitive.
//
// madc::dis is the in-memory half of madc's data substrate (the core primitives
// that ship in libmadc and that madc ITSELF uses; madc::dat is the external
// driver/serialization half). See:
//   docs/plans/madcdis-plan.md
//   docs/plans/2026-06-29-madc-development-substrate-vision.md
//
// This is the standardized INTERN-TABLE primitive, lifted from the proven
// `StringPool` the compiler already runs on (intentionally index-linked, NOT
// pointer-chained, so the three blocks — byte arena, entry arena, bucket array —
// serialize / mmap-in-place with zero fixup). Algorithm = tinycc TokenSym / SMAUG
// hashstr; the difference is indices instead of hash_next pointers, precisely so
// it embeds and round-trips through a madc::dat driver.
//
// THE INTERFACE (the contract every intern-table variant in the catalog honors):
//   uint32_t intern(bytes)      -> stable id (dedups; identical bytes => same id)
//   const char *c_str(id)       -> the interned bytes
//   uint32_t    length(id)      -> their length
//   std::string str(id)         -> a copy
//   size_t      count()         -> live entries
//   id 0 is reserved as the empty / "none" spelling.
//
// THE CATALOG (variants slot in behind this interface; build from proven use, not
// up front):
//   * intern_table          — non-counted, permanent (THIS one; literals/keywords/names)
//   * frozen_intern_table   — read-only view over serialized blocks (PCH / forest tier; below)
//   * (future) refcounted   — lifetime-managed interned values
//
// Lifetime contract: c_str(id) points into the byte arena and is valid until the
// next intern() that GROWS the arena. Hold the id, not the pointer. reserve() up
// front (a decent default below) to avoid growth/relocation in the hot path.

namespace madc {
namespace dis {

class intern_table
{
public:
    // One hash-chain record; the on-disk/entry-block record shape too (16 bytes,
    // fixed layout — frozen_intern_table binds an array of these directly).
    struct Entry { uint32_t off; uint32_t len; uint32_t hash; uint32_t next; };
private:
    // `mutable`: interning is a memoizing dedup cache — it adds a spelling but does
    // not change any OBSERVABLE state (same bytes -> same id, before and after).
    // So intern() is logically const and callable from const contexts.
    mutable std::vector<char>     _bytes;     // spelling bytes, append-only ('\0' separated)
    mutable std::vector<Entry>    _entries;   // id == index; id 0 = empty
    mutable std::vector<uint32_t> _buckets;   // power-of-two; head entry-index or NIL
    enum : uint32_t { NIL = 0xffffffffu };  // enum (prvalue) → no ODR definition needed

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
public:
    struct transaction_state {
	size_t bytes_size;
	size_t entries_size;
	transaction_state() : bytes_size(0), entries_size(0) {}
    };

    intern_table()
    {
	reserve(1u << 16, 1u << 12);  // decent default: avoid early realloc churn
	_bytes.push_back('\0');       // off 0 = the empty string's NUL
	Entry e0 = { 0, 0, 0, NIL };
	_entries.push_back(e0);       // id 0 = empty / "none"
	if ( _buckets.empty() ) _buckets.assign(1u << 10, NIL);
    }
    // Pre-size the arenas (indices stay valid across growth; this only avoids the
    // realloc memcpy + keeps c_str() pointers stable). Buckets rounded up to a
    // power of two; never shrinks.
    void reserve(size_t nbytes, size_t nentries)
    {
	_bytes.reserve(nbytes);
	_entries.reserve(nentries);
	size_t nb = 1024;
	while ( nb < nentries ) nb <<= 1;
	if ( nb > _buckets.size() ) { if ( _entries.size() <= 1 ) _buckets.assign(nb, NIL); else rehash(nb); }
    }

    // tinycc TOK_HASH_FUNC (init 1). Exposed as init + per-char step so the lexer
    // can fold the hash WHILE it reads an identifier — no second byte-pass at
    // intern(). hash_bytes() must stay identical to folding step() over the bytes.
    static uint32_t hash_init()                        { return 1; }
    static uint32_t hash_step(uint32_t h, unsigned char c) { return h + (h << 5) + (h >> 27) + c; }
    static uint32_t hash_bytes(const char *s, uint32_t len)
    {
	uint32_t h = hash_init();
	for ( uint32_t i = 0; i < len; ++i )
	    h = hash_step(h, (unsigned char)s[i]);
	return h;
    }

    // Intern bytes -> stable id (dedups). Empty -> id 0. The 3-arg form takes a
    // PRECOMPUTED hash (folded during lexing) so the bytes are walked once.
    uint32_t intern(const char *s, uint32_t len, uint32_t h) const
    {
	if ( len == 0 ) return 0;
	uint32_t mask = (uint32_t)_buckets.size() - 1;
	uint32_t b = h & mask;
	for ( uint32_t i = _buckets[b]; i != NIL; i = _entries[i].next )
	{
	    const Entry &e = _entries[i];
	    if ( e.hash == h && e.len == len && memcmp(&_bytes[e.off], s, len) == 0 )
		return i;  // dedup hit
	}
	if ( _entries.size() > _buckets.size() )  // grow at load factor 1
	{
	    rehash(_buckets.size() * 2);
	    mask = (uint32_t)_buckets.size() - 1;
	    b = h & mask;
	}
	uint32_t off = (uint32_t)_bytes.size();
	_bytes.insert(_bytes.end(), s, s + len);
	_bytes.push_back('\0');
	uint32_t id = (uint32_t)_entries.size();
	Entry e = { off, len, h, _buckets[b] };
	_entries.push_back(e);
	_buckets[b] = id;
	return id;
    }
    uint32_t intern(const char *s, uint32_t len) const { return len ? intern(s, len, hash_bytes(s, len)) : 0; }
    uint32_t intern(const std::string &s) const { return intern(s.data(), (uint32_t)s.size()); }
    uint32_t intern(const char *s)        const { return intern(s, (uint32_t)strlen(s)); }

    void begin_transaction(transaction_state &state) const
    {
	state.bytes_size = _bytes.size();
	state.entries_size = _entries.size();
    }
    void commit_transaction(transaction_state &) const {}
    void rollback_transaction(const transaction_state &state) const
    {
	_bytes.resize(state.bytes_size);
	_entries.resize(state.entries_size);
	rehash(_buckets.size());
    }

    const char *c_str(uint32_t id)  const { return &_bytes[_entries[id].off]; }
    uint32_t    length(uint32_t id) const { return _entries[id].len; }
    std::string str(uint32_t id)    const { return std::string(c_str(id), length(id)); }
    size_t      count()             const { return _entries.size() - 1; } // excl. id 0

    // --- serialization accessors: the three blocks, exactly as a madc::dat
    // writer stores them and frozen_intern_table rebinds them. The blocks are
    // index-linked (Entry.next / bucket heads are entry INDICES), so together
    // they round-trip with zero fixup. Sizes are in ELEMENTS of each block.
    const char     *bytes_data()   const { return _bytes.data(); }
    size_t          bytes_size()   const { return _bytes.size(); }
    const Entry    *entries_data() const { return _entries.data(); }
    size_t          entries_size() const { return _entries.size(); }
    const uint32_t *buckets_data() const { return _buckets.data(); }
    size_t          buckets_size() const { return _buckets.size(); }
};

// The FROZEN intern-table catalog variant: a read-only VIEW over the three
// serialized blocks placed in loaded / mmap'd memory. No fixup — the blocks are
// index-linked by construction. Lookups only: a loaded (forest / PCH) segment
// never interns; the live TU's mutable intern_table does. id 0 is the empty
// spelling, as in the live table; find() returns npos for an absent spelling.
//
// Alignment contract: the entries block must be 4-aligned and the buckets block
// 4-aligned in the bound memory (the snapshot container 16-aligns segment
// payloads, which satisfies this).
class frozen_intern_table
{
    const char                *_bytes;    size_t _nbytes;
    const intern_table::Entry *_entries;  size_t _nentries;
    const uint32_t            *_buckets;  size_t _nbuckets;  // power of two, or 0 = find() disabled
public:
    enum : uint32_t { npos = 0xffffffffu };  // absent spelling; same value as the chain NIL

    frozen_intern_table()
	: _bytes(0), _nbytes(0), _entries(0), _nentries(0), _buckets(0), _nbuckets(0) {}

    void bind(const char *bytes, size_t nbytes,
	      const intern_table::Entry *entries, size_t nentries,
	      const uint32_t *buckets, size_t nbuckets)
    {
	_bytes = bytes;     _nbytes = nbytes;
	_entries = entries; _nentries = nentries;
	_buckets = buckets; _nbuckets = nbuckets;
    }

    // Structural sanity over the bound blocks — the container-load gate. O(n)
    // bounds checks; a false return means the segment set is corrupt or
    // mismatched and must be rejected (fall back to live parse, never crash).
    bool valid() const
    {
	if ( !_bytes || !_entries || _nentries == 0 || _nbytes == 0 )
	    return false;
	if ( _entries[0].off != 0 || _entries[0].len != 0 || _bytes[0] != '\0' )
	    return false;
	for ( size_t i = 0; i < _nentries; ++i )
	{
	    const intern_table::Entry &e = _entries[i];
	    if ( (size_t)e.off + e.len + 1 > _nbytes )        // room incl. trailing NUL
		return false;
	    if ( _bytes[e.off + e.len] != '\0' )
		return false;
	    if ( e.next != npos && e.next >= _nentries )
		return false;
	}
	if ( _nbuckets )
	{
	    if ( (_nbuckets & (_nbuckets - 1)) != 0 )         // power of two
		return false;
	    for ( size_t b = 0; b < _nbuckets; ++b )
		if ( _buckets[b] != npos && _buckets[b] >= _nentries )
		    return false;
	}
	return true;
    }

    const char *c_str(uint32_t id)  const { return &_bytes[_entries[id].off]; }
    uint32_t    length(uint32_t id) const { return _entries[id].len; }
    std::string str(uint32_t id)    const { return std::string(c_str(id), length(id)); }
    size_t      count()             const { return _nentries ? _nentries - 1 : 0; }

    // Read-only lookup: id for these bytes, or npos when absent. Empty -> id 0.
    // Requires the buckets block; hash discipline identical to the live table.
    uint32_t find(const char *s, uint32_t len, uint32_t h) const
    {
	if ( len == 0 ) return 0;
	if ( !_nbuckets ) return npos;
	uint32_t b = h & ((uint32_t)_nbuckets - 1);
	for ( uint32_t i = _buckets[b]; i != npos; i = _entries[i].next )
	{
	    const intern_table::Entry &e = _entries[i];
	    if ( e.hash == h && e.len == len && memcmp(&_bytes[e.off], s, len) == 0 )
		return i;
	}
	return npos;
    }
    uint32_t find(const char *s, uint32_t len) const
	{ return len ? find(s, len, intern_table::hash_bytes(s, len)) : 0; }
    uint32_t find(const std::string &s) const { return find(s.data(), (uint32_t)s.size()); }
};

// Map keyed by interned spelling-id. A drop-in for std::map<std::string,V> on the
// hot lexer maps (keyword/define/macro/cpp-operator): the STRING overloads intern
// the key so every existing insert / cold-path call site compiles unchanged, while
// the hot lookups pass a PRE-COMPUTED spelling_id (uint32) and skip the
// std::less<string> tree-walk entirely (callgrind: that comparator was 6.7% incl /
// 4.2M calls — docs/plans/2026-06-23-arena-interning-HANDOFF.md). The pool pointer
// is bound once in Program::_tokenizer_init() before any use.
template<class V>
class intern_keyed_map
{
    std::vector<int32_t> _slot;   // indexed by spelling-id: index into _vals, or -1
    std::vector<V>       _vals;   // dense value pool (only real entries)
    intern_table        *_pool = nullptr;
    size_t               _live = 0;
public:
    // Temporary parser work needs map entries to remain visible during a
    // production parse, then disappear atomically. Mutable access saves the
    // original value once; rollback restores touched keys and truncates inserts.
    struct transaction_state
    {
	struct saved_value
	{
	    uint32_t id;
	    int32_t slot;
	    V value;
	    saved_value(uint32_t i, int32_t s, const V &v)
		: id(i), slot(s), value(v) {}
	};
	size_t slot_size;
	size_t vals_size;
	size_t live;
	std::vector<saved_value> saved;
	std::vector<uint32_t> inserted;
	transaction_state() : slot_size(0), vals_size(0), live(0) {}
    };
private:
    transaction_state   *_transaction = nullptr;

    void save_transaction_value(uint32_t id)
    {
	if ( !_transaction || id >= _slot.size() || _slot[id] < 0
	  || (size_t)_slot[id] >= _transaction->vals_size )
	    return;
	for ( size_t i = 0; i < _transaction->saved.size(); ++i )
	    if ( _transaction->saved[i].id == id )
		return;
	_transaction->saved.push_back(typename transaction_state::saved_value(
	    id, _slot[id], _vals[(size_t)_slot[id]]));
    }
public:
    typedef V *iterator;          // find() returns a pointer to the value, NULL = absent
    typedef const V *const_iterator;
    intern_keyed_map() {}
    intern_keyed_map(const intern_keyed_map &other)
	: _slot(other._slot), _vals(other._vals), _pool(other._pool),
	  _live(other._live), _transaction(nullptr) {}
    intern_keyed_map &operator=(const intern_keyed_map &other)
    {
	if ( this == &other )
	    return *this;
	assert(!_transaction);
	_slot = other._slot;
	_vals = other._vals;
	_pool = other._pool;
	_live = other._live;
	return *this;
    }
    void set_pool(intern_table *p) { _pool = p; }

    void begin_transaction(transaction_state &state)
    {
	assert(!_transaction);
	state.slot_size = _slot.size();
	state.vals_size = _vals.size();
	state.live = _live;
	state.saved.clear();
	state.inserted.clear();
	_transaction = &state;
    }

    void commit_transaction(transaction_state &state)
    {
	assert(_transaction == &state);
	_transaction = nullptr;
    }

    void rollback_transaction(transaction_state &state)
    {
	assert(_transaction == &state);
	_vals.resize(state.vals_size);
	_slot.resize(state.slot_size, -1);
	for ( size_t i = 0; i < state.inserted.size(); ++i )
	    if ( state.inserted[i] < _slot.size() )
		_slot[state.inserted[i]] = -1;
	for ( size_t i = 0; i < state.saved.size(); ++i )
	{
	    const typename transaction_state::saved_value &saved = state.saved[i];
	    _vals[(size_t)saved.slot] = saved.value;
	    _slot[saved.id] = saved.slot;
	}
	_live = state.live;
	_transaction = nullptr;
    }

    // Pre-size the dense storage. Reserving the value pool up front means it
    // does not relocate as entries are added, so a value held by reference or
    // pointer across an insert stays valid (the vector-storage analogue of
    // std::map's stable nodes). `max_id` pre-grows the slot array so early
    // operator[] calls don't resize it either. Cheap single allocation vs the
    // incremental-realloc / per-node-malloc the std::map shapes paid.
    void reserve(size_t nvals, uint32_t max_id = 0)
    {
	_vals.reserve(nvals);
	if ( max_id && (size_t)max_id + 1 > _slot.size() )
	    _slot.resize((size_t)max_id + 1, -1);
    }

    // uint32 (hot) — O(1) flat array index; no tree, no node alloc, no compare
    V *find(uint32_t id)
    {
	if ( id < _slot.size() && _slot[id] >= 0 )
	{
	    save_transaction_value(id);
	    return &_vals[(size_t)_slot[id]];
	}
	return nullptr;
    }
    // Existing parent value, with rollback owned by a narrower subvalue
    // journal. This prevents copying a large grouped value merely to mutate
    // one independently journaled child. New parent keys must still use
    // operator[] so the normal transaction records their insertion.
    V *find_for_subvalue_write(uint32_t id, transaction_state &state)
    {
	assert(_transaction == &state);
	return id < _slot.size() && _slot[id] >= 0
	    ? &_vals[(size_t)_slot[id]] : nullptr;
    }
    const V *find_readonly(uint32_t id) const
    {
	return id < _slot.size() && _slot[id] >= 0
	    ? &_vals[(size_t)_slot[id]] : nullptr;
    }
    size_t count(uint32_t id) const
    {
	return ( id < _slot.size() && _slot[id] >= 0 ) ? 1 : 0;
    }
    V &operator[](uint32_t id)
    {
	save_transaction_value(id);
	if ( id >= _slot.size() ) _slot.resize(id + 1, -1);
	if ( _slot[id] < 0 )
	{
	    if ( _transaction )
		_transaction->inserted.push_back(id);
	    _slot[id] = (int32_t)_vals.size();
	    _vals.emplace_back();
	    ++_live;
	}
	return _vals[(size_t)_slot[id]];
    }
    size_t erase(uint32_t id)
    {
	if ( id < _slot.size() && _slot[id] >= 0 )
	{
	    save_transaction_value(id);
	    _vals[(size_t)_slot[id]] = V();  // tombstone the value; pool slot is left (erase is rare)
	    _slot[id] = -1;
	    --_live;
	    return 1;
	}
	return 0;
    }

    // string overloads — intern the key; keep every existing insert / cold call site working
    V     *find(const std::string &k)        { return find(_pool->intern(k)); }
    const V *find_readonly(const std::string &k) const
	{ return find_readonly(_pool->intern(k)); }
    size_t count(const std::string &k)       { return count(_pool->intern(k)); }
    V     &operator[](const std::string &k)  { return (*this)[_pool->intern(k)]; }
    size_t erase(const std::string &k)       { return erase(_pool->intern(k)); }

    // Enumerate live entries: fn(const char *key, V &value) for each present
    // key, in ascending spelling-id order. Returning true from fn stops early
    // (so a search can break on the first hit). The key string is recovered
    // from the bound pool — valid for the duration of the callback (do not
    // intern() into this pool during enumeration). This is what lets a
    // key-iterating consumer (e.g. a "::op" suffix scan) move off std::map
    // without losing key access.
    template<class Fn>
    void for_each(Fn fn)
    {
	for ( uint32_t id = 0; id < _slot.size(); ++id )
	    if ( _slot[id] >= 0 )
	    {
		save_transaction_value(id);
		if ( fn(_pool->c_str(id), _vals[(size_t)_slot[id]]) )
		    return;
	    }
    }

    // find() returns NULL for "not found"; end() is provided so legacy
    // `find(x) != m.end()` call sites keep compiling (end() == nullptr).
    iterator end() { return nullptr; }
    bool   empty() const { return _live == 0; }
    size_t size()  const { return _live; }
    void clear()
    {
	if ( _transaction )
	    for ( uint32_t id = 0; id < _slot.size(); ++id )
		if ( _slot[id] >= 0 )
		    save_transaction_value(id);
	_slot.clear();
	_vals.clear();
	_live = 0;
    }
};

} // namespace dis
} // namespace madc

#endif
