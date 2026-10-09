#ifndef __MADCDIS_ISTRING_H
#define __MADCDIS_ISTRING_H 1

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <functional>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <deque>
#include <vector>

// madcdis/istring.h — an INTERNED NAME: a handle to one permanent,
// process-wide, deduplicated std::string. The intern-table catalog's
// permanent, process-wide variant (intern_table.h is the per-Program,
// index-linked one under token spellings).
//
// Equal strings are the same entry, so:
//   * copying an istring copies a pointer (no allocation);
//   * == / != compare pointers; std::hash<istring> hashes the pointer;
//   * c_str(), and the std::string reference istring converts to, are stable
//     for the life of the process.
// operator< compares the BYTES, so an ordered container keyed by istring
// walks in the same order as one keyed by std::string.
//
// Read-only std::string use compiles unchanged: istring converts to
// `const std::string &` (the interned entry itself — no copy) and forwards the
// common const members. Assigning a new value interns it. Mutation in place
// (+=, append, [i] =) does not compile: build the new value and assign it.
//
// Thread-safety: entries are immutable and never freed, so reading an istring
// is safe from any thread; interning is serialized by the table's mutex, which
// a single-threaded batch (the CLI compile) may take once for its whole run
// (istring_table::hold). An istring object itself follows the C++
// standard-library convention.
// Design: docs/plans/2026-10-08-compile-time-vs-gxx.md ("Interned names").

namespace madc {
namespace dis {

class istring_table
{
public:
    // The one table. Function-local static: initialized on first use,
    // thread-safely (C++11 [stmt.dcl]/4), and never destroyed, so an istring
    // held by any static object stays valid through process exit.
    static istring_table &instance()
    {
	static istring_table *t = new istring_table();
	return *t;
    }
    const std::string *intern(const char *s, size_t n)
    {
	if ( n == 0 )
	    return empty();
	if ( _owner.load(std::memory_order_relaxed) == std::this_thread::get_id() )
	    return lookup_or_insert(s, n);	// this thread holds the table
	std::lock_guard<std::mutex> lock(_mu);
	return lookup_or_insert(s, n);
    }

    // A HOLD: one thread takes the table's mutex for a whole batch of interning
    // (the CLI's tokenize + parse of a compile) and interns without locking per
    // call. Another thread that interns meanwhile takes the mutex as usual and
    // waits for the hold to end. Nested holds on the owning thread are no-ops.
    // Never hold across running a program: its threads may compile (eval) and
    // a holder that waits on them would never release.
    class hold
    {
    public:
	hold() : _t(instance()), _took(false)
	{
	    if ( _t._owner.load(std::memory_order_relaxed) == std::this_thread::get_id() )
		return;
	    _t._mu.lock();
	    _t._owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
	    _took = true;
	}
	~hold()
	{
	    if ( !_took )
		return;
	    _t._owner.store(std::thread::id(), std::memory_order_relaxed);
	    _t._mu.unlock();
	}
    private:
	hold(const hold &);
	hold &operator=(const hold &);
	istring_table &_t;
	bool _took;
    };
    static const std::string *empty()
    {
	static const std::string *e = new std::string();
	return e;
    }
    size_t count()
    {
	if ( _owner.load(std::memory_order_relaxed) == std::this_thread::get_id() )
	    return _count;
	std::lock_guard<std::mutex> lock(_mu);
	return _count;
    }
private:
    // Eight bytes per step: each word is folded in with a rotate-xor-multiply,
    // the tail (< 8 bytes) as one zero-padded word, and the length; then a
    // murmur3 finalizer spreads the bits for the slot index.
    static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
    static size_t hash_bytes(const char *p, size_t n)
    {
	const uint64_t K = 0x9E3779B97F4A7C15ULL;
	uint64_t h = n * K;
	for ( ; n >= 8; p += 8, n -= 8 )
	{
	    uint64_t w;
	    memcpy(&w, p, 8);
	    h = (rotl(h, 5) ^ w) * K;
	}
	// The 1..7-byte tail (most whole names): fixed-size loads only, so each
	// is one instruction, never a variable-length memcpy call. 4..7 bytes are
	// two overlapping 4-byte reads; 1..3 are the first, middle and last byte,
	// which cover every byte. The length is already in the seed, so the
	// overlap cannot make two spellings of different lengths collide.
	if ( n >= 4 )
	{
	    uint32_t a, b;
	    memcpy(&a, p, 4);
	    memcpy(&b, p + n - 4, 4);
	    h = (rotl(h, 5) ^ (((uint64_t)a << 32) | b)) * K;
	}
	else if ( n )
	{
	    const uint64_t w = ((uint64_t)(unsigned char)p[0] << 16)
			     | ((uint64_t)(unsigned char)p[n >> 1] << 8)
			     | (uint64_t)(unsigned char)p[n - 1];
	    h = (rotl(h, 5) ^ w) * K;
	}
	h ^= h >> 33;
	h *= 0xFF51AFD7ED558CCDULL;
	h ^= h >> 33;
	h *= 0xC4CEB9FE1A85EC53ULL;
	h ^= h >> 33;
	return (size_t)h;
    }
    // The index: open addressing, linear probing, at most half full; a slot
    // carries the full hash so a probe compares bytes only on a hash match.
    // Entries live in an ARENA (a deque grows in blocks and never moves an
    // element), so a new name costs no per-name allocation beyond the
    // characters of one longer than the std::string inline buffer.
    struct Slot {
	size_t hash;
	const std::string *entry;	// NULL = empty
    };
    istring_table() : _owner(std::thread::id()), _count(0)
    {
	Slot none = { 0, NULL };
	_slots.assign(1u << 15, none);
    }
    void place(size_t h, const std::string *e)
    {
	size_t mask = _slots.size() - 1;
	size_t i = h & mask;
	while ( _slots[i].entry )
	    i = (i + 1) & mask;
	_slots[i].hash = h;
	_slots[i].entry = e;
    }
    void grow()
    {
	std::vector<Slot> old;
	old.swap(_slots);
	Slot none = { 0, NULL };
	_slots.assign(old.size() * 2, none);
	for ( size_t i = 0; i < old.size(); ++i )
	    if ( old[i].entry )
		place(old[i].hash, old[i].entry);
    }
    // Caller holds _mu (a lock_guard, or a hold on this thread).
    const std::string *lookup_or_insert(const char *s, size_t n)
    {
	size_t h = hash_bytes(s, n);
	size_t mask = _slots.size() - 1;
	for ( size_t i = h & mask; _slots[i].entry; i = (i + 1) & mask )
	{
	    const Slot &sl = _slots[i];
	    if ( sl.hash == h && sl.entry->size() == n
	      && memcmp(sl.entry->data(), s, n) == 0 )
		return sl.entry;
	}
	_store.emplace_back(s, n);		// permanent: never freed
	const std::string *e = &_store.back();
	if ( (_count + 1) * 2 > _slots.size() )
	    grow();
	place(h, e);
	++_count;
	return e;
    }
    std::mutex _mu;
    // The thread holding _mu through a hold (a default id = none).
    std::atomic<std::thread::id> _owner;
    std::deque<std::string> _store;
    std::vector<Slot> _slots;
    size_t _count;
};

class istring
{
public:
    typedef std::string::size_type size_type;
    static const size_type npos = std::string::npos;

    istring() : _p(istring_table::empty()) {}
    istring(const char *s)
	: _p(s ? istring_table::instance().intern(s, strlen(s))
	       : istring_table::empty()) {}
    istring(const char *s, size_t n) : _p(istring_table::instance().intern(s, n)) {}
    istring(const std::string &s)
	: _p(istring_table::instance().intern(s.data(), s.size())) {}
    // A name made from part of other text: the substring / range / repeat is
    // built, then interned (std::string's matching constructors).
    istring(const std::string &s, size_t pos, size_t n = std::string::npos)
	: istring(s.substr(pos, n)) {}
    // One character (a punctuator's spelling) comes from a table interned
    // once, not re-hashed per token.
    istring(size_t n, char c)
	: _p(n == 1 ? single_char(c)
		    : istring_table::instance().intern(std::string(n, c).data(), n)) {}
    template <class It>
    istring(It first, It last) : istring(std::string(first, last)) {}

    // Rebind to the empty name (the handle changes; no entry is modified).
    void clear() { _p = istring_table::empty(); }

    // The interned entry itself: no copy.
    const std::string &str() const { return *_p; }
    operator const std::string &() const { return *_p; }

    const char *c_str() const { return _p->c_str(); }
    const char *data() const { return _p->data(); }
    size_type size() const { return _p->size(); }
    size_type length() const { return _p->size(); }
    bool empty() const { return _p->empty(); }
    char operator[](size_type i) const { return (*_p)[i]; }
    char front() const { return _p->front(); }
    char back() const { return _p->back(); }
    std::string::const_iterator begin() const { return _p->begin(); }
    std::string::const_iterator end() const { return _p->end(); }
    std::string substr(size_type pos = 0, size_type n = npos) const { return _p->substr(pos, n); }
    size_type find(const std::string &s, size_type pos = 0) const { return _p->find(s, pos); }
    size_type find(const char *s, size_type pos = 0) const { return _p->find(s, pos); }
    size_type find(char c, size_type pos = 0) const { return _p->find(c, pos); }
    size_type rfind(const std::string &s, size_type pos = npos) const { return _p->rfind(s, pos); }
    size_type rfind(const char *s, size_type pos = npos) const { return _p->rfind(s, pos); }
    size_type rfind(char c, size_type pos = npos) const { return _p->rfind(c, pos); }
    std::string::const_reverse_iterator rbegin() const { return _p->rbegin(); }
    std::string::const_reverse_iterator rend() const { return _p->rend(); }
    size_type find(const char *s, size_type pos, size_type n) const { return _p->find(s, pos, n); }
    size_type find_first_of(const std::string &s, size_type pos = 0) const { return _p->find_first_of(s, pos); }
    size_type find_first_of(const char *s, size_type pos = 0) const { return _p->find_first_of(s, pos); }
    size_type find_first_of(char c, size_type pos = 0) const { return _p->find_first_of(c, pos); }
    size_type find_last_of(const std::string &s, size_type pos = npos) const { return _p->find_last_of(s, pos); }
    size_type find_last_of(const char *s, size_type pos = npos) const { return _p->find_last_of(s, pos); }
    size_type find_last_of(char c, size_type pos = npos) const { return _p->find_last_of(c, pos); }
    size_type find_first_not_of(const std::string &s, size_type pos = 0) const { return _p->find_first_not_of(s, pos); }
    size_type find_first_not_of(const char *s, size_type pos = 0) const { return _p->find_first_not_of(s, pos); }
    size_type find_first_not_of(char c, size_type pos = 0) const { return _p->find_first_not_of(c, pos); }
    size_type find_last_not_of(const std::string &s, size_type pos = npos) const { return _p->find_last_not_of(s, pos); }
    size_type find_last_not_of(const char *s, size_type pos = npos) const { return _p->find_last_not_of(s, pos); }
    size_type find_last_not_of(char c, size_type pos = npos) const { return _p->find_last_not_of(c, pos); }
    // Exchange the two handles (no entry is modified).
    void swap(istring &o) { const std::string *t = _p; _p = o._p; o._p = t; }
    int compare(const std::string &s) const { return _p->compare(s); }
    int compare(const char *s) const { return _p->compare(s); }
    int compare(size_type pos, size_type n, const std::string &s) const { return _p->compare(pos, n, s); }
    int compare(size_type pos, size_type n, const char *s) const { return _p->compare(pos, n, s); }

    // Identity: the entry pointer.
    const std::string *entry() const { return _p; }

    friend bool operator==(const istring &a, const istring &b) { return a._p == b._p; }
    friend bool operator!=(const istring &a, const istring &b) { return a._p != b._p; }
    friend bool operator<(const istring &a, const istring &b) { return a._p != b._p && *a._p < *b._p; }
    friend bool operator>(const istring &a, const istring &b) { return b < a; }
    friend bool operator<=(const istring &a, const istring &b) { return !(b < a); }
    friend bool operator>=(const istring &a, const istring &b) { return !(a < b); }
    friend bool operator==(const istring &a, const std::string &b) { return *a._p == b; }
    friend bool operator==(const std::string &a, const istring &b) { return a == *b._p; }
    friend bool operator!=(const istring &a, const std::string &b) { return *a._p != b; }
    friend bool operator!=(const std::string &a, const istring &b) { return a != *b._p; }
    friend bool operator==(const istring &a, const char *b) { return *a._p == b; }
    friend bool operator==(const char *a, const istring &b) { return a == *b._p; }
    friend bool operator!=(const istring &a, const char *b) { return *a._p != b; }
    friend bool operator!=(const char *a, const istring &b) { return a != *b._p; }
    friend bool operator<(const istring &a, const std::string &b) { return *a._p < b; }
    friend bool operator<(const std::string &a, const istring &b) { return a < *b._p; }

    friend std::string operator+(const istring &a, const istring &b) { return *a._p + *b._p; }
    friend std::string operator+(const istring &a, const std::string &b) { return *a._p + b; }
    friend std::string operator+(const std::string &a, const istring &b) { return a + *b._p; }
    friend std::string operator+(const istring &a, const char *b) { return *a._p + b; }
    friend std::string operator+(const char *a, const istring &b) { return a + *b._p; }
    friend std::string operator+(const istring &a, char b) { return *a._p + b; }
    friend std::string operator+(char a, const istring &b) { return a + *b._p; }
    friend std::string operator+(std::string &&a, const istring &b) { return std::move(a) + *b._p; }

    friend std::ostream &operator<<(std::ostream &os, const istring &s) { return os << *s._p; }
private:
    static const std::string *single_char(char c)
    {
	// Function-local static: built once, thread-safely ([stmt.dcl]/4).
	struct Table
	{
	    const std::string *entry[256];
	    Table()
	    {
		for ( int i = 0; i < 256; ++i )
		{
		    const char ch = (char)i;
		    entry[i] = istring_table::instance().intern(&ch, 1);
		}
	    }
	};
	static const Table table;
	return table.entry[(unsigned char)c];
    }

    const std::string *_p;
};

} // namespace dis
} // namespace madc

// A string LITERAL as an istring, interned once per call site (a function-
// local static) instead of on every evaluation: for a literal handed to an
// istring parameter on a hot path.
#define MADC_ISTRING_LITERAL(lit) \
    ([]() -> const ::madc::dis::istring & { \
	static const ::madc::dis::istring s_(lit); return s_; }())

namespace std {
template <> struct hash<madc::dis::istring> {
    size_t operator()(const madc::dis::istring &s) const
    {
	return std::hash<const std::string *>()(s.entry());
    }
};
}

#endif
