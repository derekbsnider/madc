#ifndef __MADCDIS_POD_RECORD_H
#define __MADCDIS_POD_RECORD_H 1

#include <cstdint>
#include <cstring>
#include <vector>

// madcdis/pod_record.h — fixed-stride POD (de)serialization over a uint32 buffer.
//
// The smallest madc::dis serialization primitive: append a trivially-copyable
// record's words to a std::vector<uint32_t>, and read one back at a word offset
// with a bounds check. Every madc::dis payload that packs an array of fixed-size
// POD records into a flat uint32 stream (the forest type-payload's member / base /
// method runs are the first consumers) shares these two calls instead of
// hand-rolling the copy loop and the offset+bounds arithmetic each time.
//
// DataDef-agnostic on purpose — pure buffer + POD, no compiler/type-system
// knowledge — so it stays a general substrate utility (see
// docs/plans/2026-07-06-madcdis-export-surface.md). The record type T must be
// trivially copyable and a whole number of uint32 words wide; both are checked
// at compile time.
//
// Word order and stride are exactly memcpy-of-the-struct, so a buffer built with
// pod_append reads back byte-identically with pod_read — the wire format is the
// struct's in-memory layout, unchanged.
//
// THE INTERFACE:
//   pod_words<T>()               -> record width in uint32 words (the stride)
//   pod_append(buf, rec) -> off  -> append rec's words; return its start word offset
//   pod_read(buf, off, out)      -> read a T at word offset off; false if OOB

namespace madc {
namespace dis {

// Record stride in uint32 words. Also the multiplier for indexing an array of
// records packed back-to-back at a known base offset.
template <typename T>
inline size_t pod_words()
{
	return sizeof(T) / sizeof(uint32_t);
}

// Append rec to buf as sizeof(T)/4 little-endian-in-memory words; return the
// word offset at which it starts (buf.size() before the append).
template <typename T>
inline uint32_t pod_append(std::vector<uint32_t> &buf, const T &rec)
{
	static_assert(sizeof(T) % sizeof(uint32_t) == 0,
		      "pod_append: record must be a whole number of uint32 words");
	uint32_t off = (uint32_t)buf.size();
	const uint32_t *w = (const uint32_t *)&rec;
	for (size_t k = 0; k < sizeof(T) / sizeof(uint32_t); ++k)
		buf.push_back(w[k]);
	return off;
}

// Read a T from a word span at word offset off. Returns false (leaving out
// untouched) when the record would run past the end of the span — the one
// bounds check every load site owes a possibly-corrupt buffer. The span form
// is the real implementation: buffers bound zero-copy from a mapped image or
// a process-lifetime decode cache read through it without owning a vector.
template <typename T>
inline bool pod_read(const uint32_t *buf, size_t nwords, size_t off, T &out)
{
	static_assert(sizeof(T) % sizeof(uint32_t) == 0,
		      "pod_read: record must be a whole number of uint32 words");
	if (off + sizeof(T) / sizeof(uint32_t) > nwords)
		return false;
	memcpy(&out, buf + off, sizeof(T));
	return true;
}

template <typename T>
inline bool pod_read(const std::vector<uint32_t> &buf, size_t off, T &out)
{
	return pod_read(buf.data(), buf.size(), off, out);
}

} // namespace dis
} // namespace madc

#endif
