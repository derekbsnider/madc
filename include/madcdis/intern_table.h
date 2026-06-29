#ifndef __MADCDIS_INTERN_TABLE_H
#define __MADCDIS_INTERN_TABLE_H 1

#include <cstdint>
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
//   * (future) refcounted   — lifetime-managed interned values
//   * (future) frozen       — read-only mmap'd table (PCH / forest tier)
//
// Lifetime contract: c_str(id) points into the byte arena and is valid until the
// next intern() that GROWS the arena. Hold the id, not the pointer. reserve() up
// front (a decent default below) to avoid growth/relocation in the hot path.

namespace madc {
namespace dis {

class intern_table
{
    struct Entry { uint32_t off; uint32_t len; uint32_t hash; uint32_t next; };
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

    const char *c_str(uint32_t id)  const { return &_bytes[_entries[id].off]; }
    uint32_t    length(uint32_t id) const { return _entries[id].len; }
    std::string str(uint32_t id)    const { return std::string(c_str(id), length(id)); }
    size_t      count()             const { return _entries.size() - 1; } // excl. id 0
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
    typedef V *iterator;          // find() returns a pointer to the value, NULL = absent
    void set_pool(intern_table *p) { _pool = p; }

    // uint32 (hot) — O(1) flat array index; no tree, no node alloc, no compare
    V *find(uint32_t id)
    {
	if ( id < _slot.size() && _slot[id] >= 0 ) return &_vals[(size_t)_slot[id]];
	return nullptr;
    }
    size_t count(uint32_t id) const
    {
	return ( id < _slot.size() && _slot[id] >= 0 ) ? 1 : 0;
    }
    V &operator[](uint32_t id)
    {
	if ( id >= _slot.size() ) _slot.resize(id + 1, -1);
	if ( _slot[id] < 0 ) { _slot[id] = (int32_t)_vals.size(); _vals.emplace_back(); ++_live; }
	return _vals[(size_t)_slot[id]];
    }
    size_t erase(uint32_t id)
    {
	if ( id < _slot.size() && _slot[id] >= 0 )
	{
	    _vals[(size_t)_slot[id]] = V();  // tombstone the value; pool slot is left (erase is rare)
	    _slot[id] = -1;
	    --_live;
	    return 1;
	}
	return 0;
    }

    // string overloads — intern the key; keep every existing insert / cold call site working
    V     *find(const std::string &k)        { return find(_pool->intern(k)); }
    size_t count(const std::string &k)       { return count(_pool->intern(k)); }
    V     &operator[](const std::string &k)  { return (*this)[_pool->intern(k)]; }
    size_t erase(const std::string &k)       { return erase(_pool->intern(k)); }

    // find() returns NULL for "not found"; end() is provided so legacy
    // `find(x) != m.end()` call sites keep compiling (end() == nullptr).
    iterator end() { return nullptr; }
    bool   empty() const { return _live == 0; }
    size_t size()  const { return _live; }
    void   clear()       { _slot.clear(); _vals.clear(); _live = 0; }
};

} // namespace dis
} // namespace madc

#endif
