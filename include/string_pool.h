#ifndef __STRING_POOL_H
#define __STRING_POOL_H 1

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

// Arena-model interned string table — the backing store for TokenRec.spelling_id
// and every interned name (sibling of the segmented type table). Index-linked, NOT
// pointer-chained, so the three blocks (byte arena, entry arena, bucket array)
// serialize / mmap-in-place with zero fixup (the cir_node-backbone discipline in
// docs/plans/2026-06-13-embedded-ast-frontend-design.md §2/§4/§5). Algorithm =
// tinycc TokenSym / SMAUG hashstr; the difference is indices instead of hash_next
// pointers, precisely so it embeds.
//
// id 0 is reserved as the empty/"none" spelling.
//
// Lifetime contract: c_str(id) points into the byte arena and is valid until the
// next intern() that GROWS the arena. Hold the id, not the pointer. reserve()
// up front (a decent default below) to avoid growth/relocation in the hot path.
class StringPool
{
    struct Entry { uint32_t off; uint32_t len; uint32_t hash; uint32_t next; };
    std::vector<char>     _bytes;     // spelling bytes, append-only ('\0' separated)
    std::vector<Entry>    _entries;   // id == index; id 0 = empty
    std::vector<uint32_t> _buckets;   // power-of-two; head entry-index or NIL
    enum : uint32_t { NIL = 0xffffffffu };  // enum (prvalue) → no ODR definition needed

    // tinycc TOK_HASH_FUNC, init 1.
    static uint32_t hash_bytes(const char *s, uint32_t len)
    {
	uint32_t h = 1;
	for ( uint32_t i = 0; i < len; ++i )
	    h = h + (h << 5) + (h >> 27) + (unsigned char)s[i];
	return h;
    }
    void rehash(size_t nbuckets)
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
    StringPool()
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

    // Intern bytes -> stable id (dedups). Empty -> id 0.
    uint32_t intern(const char *s, uint32_t len)
    {
	if ( len == 0 ) return 0;
	uint32_t h = hash_bytes(s, len);
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
    uint32_t intern(const std::string &s) { return intern(s.data(), (uint32_t)s.size()); }
    uint32_t intern(const char *s)        { return intern(s, (uint32_t)strlen(s)); }

    const char *c_str(uint32_t id)  const { return &_bytes[_entries[id].off]; }
    uint32_t    length(uint32_t id) const { return _entries[id].len; }
    std::string str(uint32_t id)    const { return std::string(c_str(id), length(id)); }
    size_t      count()             const { return _entries.size() - 1; } // excl. id 0
};

#endif
