#ifndef __MADCDIS_LAZY_BLOCKS_H
#define __MADCDIS_LAZY_BLOCKS_H 1

#include <atomic>
#include <cstddef>
#include <cstdint>

// madcdis/lazy_blocks.h — a byte span decoded block by block on first touch:
// the reader half of a BLOCKED container segment (snapshot.h
// SNAP_XFORM_BLOCKED, the zstd-seekable / clang-PCH offset-addressed model).
//
// data() is the whole span's base address from construction; a block's bytes
// are valid once ensure() has returned true for a range that covers them.
// Typed views over container tables (frozen_intern_table, FrozenDefArena)
// carry an optional pointer to one and call ensure() before each read — NULL
// means the span is fully resident (a raw image, a decoded copy, a live table).
//
// Thread contract (thread-safety.md): ensure() is safe from any thread. A
// block's bytes are written once, under the owner's lock, before its ready
// flag is published (release); a reader that observes the flag (acquire)
// observes the bytes. Bytes are never rewritten or freed while the owner
// lives — owners are process-lifetime (the forest's decoded-segment cache).

namespace madc {
namespace dis {

class lazy_blocks;

// An observer of the reads a span serves — the pack step's layout recorder
// (docs/plans/2026-10-13-pack-slices-design.md §3, format 60): once attached
// it is told every ensure() range, and it outlives the attachment (it detaches
// before it is destroyed). Ordinary runs attach none.
class lazy_touch_sink
{
public:
    virtual ~lazy_touch_sink() {}
    virtual void touched(const lazy_blocks *span, size_t off, size_t len) = 0;
};

class lazy_blocks
{
protected:
    uint8_t               *_buf;	// size() bytes; a block valid once ready
    size_t                 _size;
    uint32_t               _shift;	// log2 of the block size
    size_t                 _nblocks;
    std::atomic<uint8_t>  *_ready;	// one flag per block
    mutable std::atomic<size_t> _decoded;	// blocks decoded so far
    mutable std::atomic<lazy_touch_sink *> _sink;	// NULL: no observer

    // Slow path: decode every not-yet-ready block in [first, last] and
    // publish each. False on a decode failure (corrupt payload).
    virtual bool fill(size_t first, size_t last) const = 0;

    lazy_blocks() : _buf(0), _size(0), _shift(0), _nblocks(0), _ready(0), _decoded(0),
		    _sink(nullptr) {}
    lazy_blocks(const lazy_blocks &);
    lazy_blocks &operator=(const lazy_blocks &);
public:
    virtual ~lazy_blocks() {}

    const uint8_t *data() const { return _buf; }
    size_t size() const { return _size; }
    // The --show-stats observable: how much of the span a run decoded.
    size_t block_count() const { return _nblocks; }
    size_t blocks_decoded() const { return _decoded.load(std::memory_order_relaxed); }
    // Attach (or, with NULL, detach) the read observer.
    void observe(lazy_touch_sink *s) const { _sink.store(s, std::memory_order_release); }

    // Make [off, off+len) readable. False when the range lies outside the
    // span or a covering block fails to decode — the caller treats the read
    // as absent, exactly as an out-of-range id.
    bool ensure(size_t off, size_t len) const
    {
	if ( off > _size || len > _size - off )
	    return false;
	if ( lazy_touch_sink *s = _sink.load(std::memory_order_acquire) )
	    s->touched(this, off, len);
	if ( !len )
	    return true;
	size_t b = off >> _shift, e = (off + len - 1) >> _shift;
	for ( ; b <= e; ++b )
	    if ( !_ready[b].load(std::memory_order_acquire) )
		return fill(b, e);
	return true;
    }
};

// The ensure step of a view that may or may not be lazy: a NULL source is a
// resident span (the bounds stay the caller's own check).
inline bool lazy_ensure(const lazy_blocks *lz, size_t off, size_t len)
{
    return !lz || lz->ensure(off, len);
}

} // namespace dis
} // namespace madc

#endif // __MADCDIS_LAZY_BLOCKS_H
