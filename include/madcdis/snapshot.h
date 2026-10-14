#ifndef __MADCDIS_SNAPSHOT_H
#define __MADCDIS_SNAPSHOT_H 1

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>
#include <string>

#include "madc_pch.h"	// PchCompression + madc_pch::compress/decompress — the ONE
			// in-tree compression vocabulary/implementation (zstd
			// preferred when built with HAVE_ZSTD, zlib fallback).
#include "pod_alloc.h"	// decode_bytes — resize-without-zero-fill destinations
#include "madcdis/lazy_blocks.h"	// lazy_blocks — the BLOCKED segment reader's base
#include <atomic>
#include <mutex>

// madcdis/snapshot.h — the madc::dis pool-snapshot CONTAINER.
//
// ONE format family, two placements (docs/plans/2026-07-04-data-substrate-
// first-customer-PLAN.md §0/§4; forest format decisions SETTLED in
// docs/plans/2026-06-22-embedded-header-forest-execution-plan.md):
//
//   [snapshot_header][segment payloads (compressed frames, 16-aligned)]
//   [snapshot_segment directory][snapshot_footer]                <- end of file
//
//   * Placement 1 — standalone file: the madc::dat `snapshot://` persistence
//     form ("a pool's bytes are the canonical serialization format").
//   * Placement 2 — appended to an existing binary (the embedded header
//     forest): the footer sits at the very END of the file, carries the total
//     blob size, and so locates the blob without any external index. No magic
//     at EOF -> no blob -> the consumer falls back (live parse).
//
// The container is CONTENT-BLIND: a segment is (seg_id, kind, bytes). What the
// bytes mean is the consumer's contract via `kind` (e.g. the three
// intern-table blocks, a frozen id_table segment, flat cir_node records).
//
// Layout discipline: fixed-size little-endian PODs (x86-64 first; a byte-order
// field is reserved for later ports). Segment payload offsets are 16-aligned
// within the blob so a payload mmap'd/read at a 16-aligned blob base can be
// bound in place by typed views (frozen_intern_table et al). append_file()
// pads the target file to 16 before the blob for exactly this reason.
// dir_offset/context-hash live in the header AND the sizes in the footer so a
// reader can start from either end.

namespace madc {
namespace dis {

// v2: segment `flags` carries the byte-stream TRANSFORM vocabulary below.
// Readers accept v1 (whose flags are always 0) and v2.
enum : uint32_t { SNAPSHOT_FORMAT_VERSION = 2 };

// Reserved well-known segment `kind` tags. Consumers may use any value >=
// SNAP_KIND_CONSUMER for their own contracts; the container never interprets it.
enum snapshot_kind : uint32_t
{
    SNAP_KIND_RAW            = 0,	// opaque bytes
    SNAP_KIND_INTERN_BYTES   = 1,	// intern_table byte block
    SNAP_KIND_INTERN_ENTRIES = 2,	// intern_table Entry block
    SNAP_KIND_INTERN_BUCKETS = 3,	// intern_table bucket block
    SNAP_KIND_INTERN_OFFSETS = 4,	// intern_table offsets block (n + 1 words)
    SNAP_KIND_CONSUMER       = 256	// first consumer-defined kind
};

// Byte-stream TRANSFORMS (segment `flags`): reversible re-codings applied to
// the raw payload BEFORE compression and inverted by read_segment() after
// decompression. Content-blind, like `codec` — the consumer picks a transform
// per segment where it knows the payload SHAPE pays (delta for
// near-sequential u32 index streams, byte-plane for arrays of fixed-stride
// records whose per-field redundancy is columnar). A transformed segment
// never binds in place (raw_ptr() = NULL), so transforms are out of bounds
// for zero-copy spine segments.
//
// `flags` layout: bits 0..7 transform id (snapshot_transform), bits 8..23
// transform parameter (SNAP_XFORM_BYTEPLANE: the record stride), bits 24..31
// reserved (0) — except SNAP_XFORM_BLOCKED, whose parameter word is laid out
// by snap_blocked_flags().
enum snapshot_transform : uint32_t
{
    SNAP_XFORM_NONE      = 0,
    SNAP_XFORM_DELTA32   = 1,	// forward delta over a u32 stream (raw_size % 4 == 0)
    SNAP_XFORM_BYTEPLANE = 2,	// fixed-stride records split into byte planes
				// (param = stride > 0, raw_size % stride == 0)
    SNAP_XFORM_BLOCKED   = 3	// split into fixed-size blocks, each compressed
				// alone (snap_blocked_flags: log2 block size,
				// 10..24, and an INNER transform recoding each
				// compressed block); the payload is a
				// snapshot_block_header, the snapshot_block
				// table, then the stored blocks. Decodable block
				// by block (snapshot_lazy_segment).
};

// SNAP_XFORM_BLOCKED payload layout. A block whose stored size equals its raw
// size is stored raw (codec None — a block under SNAP_BLOCK_RAW_FLOOR bytes,
// or one the codec did not shrink); every other block is one frame of the
// segment's codec. Offsets are payload-relative.
enum : uint32_t { SNAP_BLOCK_RAW_FLOOR = 1024 };
struct snapshot_block_header { uint32_t block_shift; uint32_t block_count; };
struct snapshot_block { uint32_t offset; uint32_t stored; };

inline uint32_t snap_xform_flags(snapshot_transform id, uint32_t param = 0)
	{ return (uint32_t)id | (param << 8); }
inline uint32_t snap_xform_id(uint32_t flags)    { return flags & 0xffu; }
inline uint32_t snap_xform_param(uint32_t flags) { return (flags >> 8) & 0xffffu; }

// SNAP_XFORM_BLOCKED's flags: bits 8..15 log2 block size, bits 24..31 the
// INNER transform recoding each compressed block before its frame
// (SNAP_XFORM_NONE, DELTA32 over the block's u32 words, or BYTEPLANE over
// the block's whole records of bits 16..23 bytes — a block's tail shorter
// than one record stays as it is). A block stored raw carries no inner
// recoding. Decoded, every block is its raw bytes: the inner transform is
// the codec's business, never a reader's.
inline uint32_t snap_blocked_flags(uint32_t shift,
				   snapshot_transform inner = SNAP_XFORM_NONE,
				   uint32_t stride = 0)
	{ return (uint32_t)SNAP_XFORM_BLOCKED | (shift << 8) | (stride << 16)
		 | ((uint32_t)inner << 24); }
inline uint32_t snap_blocked_shift(uint32_t flags)  { return (flags >> 8) & 0xffu; }
inline uint32_t snap_blocked_stride(uint32_t flags) { return (flags >> 16) & 0xffu; }
inline uint32_t snap_blocked_inner(uint32_t flags)  { return flags >> 24; }

struct snapshot_header		// at blob offset 0 (32 bytes)
{
    char     magic[8];		// "MADCSNAP"
    uint32_t version;		// SNAPSHOT_FORMAT_VERSION
    uint32_t pool_kind;		// reserved (0); pool/backing discriminator later
    uint64_t context_hash;	// pin: reject-and-fall-back on mismatch (0 = unpinned)
    uint32_t segment_count;
    uint32_t header_bytes;	// sizeof(snapshot_header) — forward-compat skip
};

struct snapshot_segment		// one directory record (40 bytes)
{
    uint32_t seg_id;		// consumer-assigned, unique within the container
    uint32_t kind;		// snapshot_kind or consumer-defined tag
    uint64_t offset;		// blob-relative payload offset (16-aligned)
    uint64_t comp_size;		// stored payload bytes
    uint64_t raw_size;		// decompressed bytes (== comp_size when codec None)
    uint32_t codec;		// PchCompression value
    uint32_t flags;		// transform vocabulary (snapshot_transform; 0 = none)
};

struct snapshot_footer		// the LAST bytes of the file (32 bytes)
{
    uint64_t dir_offset;	// blob-relative offset of snapshot_segment[segment_count]
    uint64_t blob_size;		// total container bytes (header..footer inclusive)
    uint32_t segment_count;
    uint32_t version;
    char     magic[8];		// "MADCSNAP" — checked first, from EOF
};

// Assembles a container in memory; emits it as a standalone file or appended
// to an existing binary. Payloads are compressed at add_segment() time.
class snapshot_writer
{
    struct Pending { snapshot_segment meta; std::vector<uint8_t> payload; };
    std::vector<Pending> _segs;
    uint64_t _context_hash;
public:
    snapshot_writer() : _context_hash(0) {}

    void set_context_hash(uint64_t h) { _context_hash = h; }

    // Compress and stage one segment. False on duplicate seg_id, codec
    // failure (e.g. Zstd requested in a build without HAVE_ZSTD), or a
    // transform that doesn't fit the payload (size not a multiple of the
    // element/stride). `level` is the Zstd compression level (0 = codec
    // default; see madc_pch::compress) — decompression speed is
    // level-independent. `xform` is a snap_xform_flags() value applied to
    // the payload before compression (read_segment inverts it). For a
    // SNAP_XFORM_BLOCKED segment, `raw_blocks` (optional) marks the blocks to
    // store uncompressed — read by most binds, a copy costs less than a decode.
    bool add_segment(uint32_t seg_id, uint32_t kind,
		     const void *bytes, size_t len, PchCompression codec,
		     int level = 0, uint32_t xform = 0,
		     const std::vector<bool> *raw_blocks = 0);

    // Carry a segment over from another container unchanged: its directory
    // record (offset reassigned at build()) and its stored payload bytes,
    // already encoded. False on a duplicate seg_id.
    bool add_encoded_segment(const snapshot_segment &meta, const void *payload);

    // Assemble the complete blob (header + payloads + directory + footer).
    bool build(std::vector<uint8_t> &out) const;

    bool write_file(const char *path) const;	// create/truncate (placement 1)
    bool append_file(const char *path) const;	// pad-to-16 + append (placement 2)

    size_t segment_count() const { return _segs.size(); }
};

// Append a PRE-BUILT container blob to an existing file — the placement-2
// shape (pad the host to 16 so interior 16-aligned payload offsets stay
// aligned, then the blob; the footer lands at EOF). The bytes must already
// be a complete container (snapshot_writer::build's output, or a standalone
// container file read verbatim). append_file rides this; --pack-forest's
// ELF carrier arm calls it directly with a container file's bytes.
bool snapshot_append_blob(const char *path, const void *blob, size_t len);

// The lazy reader of one SNAP_XFORM_BLOCKED segment (lazy_blocks.h): the
// segment's raw bytes at one address, each block decoded on first ensure().
// Holds only the payload bytes (a process-lifetime image mapping) and the
// segment record — never the reader — so it may outlive the reader that made
// it (snapshot_reader::open_lazy).
class snapshot_lazy_segment : public lazy_blocks
{
    const uint8_t *_payload;
    uint32_t       _codec;
    uint32_t       _flags;		// the segment's (snap_blocked_flags)
    uint32_t       _seg_id;
    mutable std::mutex _mu;
    bool fill(size_t first, size_t last) const;
    snapshot_lazy_segment(const uint8_t *payload, const snapshot_segment &seg);
    friend class snapshot_reader;
public:
    ~snapshot_lazy_segment();
    uint32_t seg_id() const { return _seg_id; }
};

// Opens a container over a memory image whose LAST bytes are the footer — a
// standalone snapshot file, or a whole executable with the blob appended. The
// image must stay live for the reader's lifetime (raw_ptr/read_segment read
// from it). The directory is read in place when aligned in the image and
// copied out otherwise, so interior alignment is never assumed.
class snapshot_reader
{
	const uint8_t *_image;	// whole mapped carrier passed to open()
	size_t _image_size;
    const uint8_t *_blob;	// blob start within the image
    size_t _blob_size;
    snapshot_header _hdr;
    // The directory: read in place in the image (_dir_image) when aligned
    // there, else copied out (_dir_copy); _dir_end bounds every payload.
    const snapshot_segment *_dir_image;
    std::vector<snapshot_segment> _dir_copy;
    uint32_t _dir_n;
    uint64_t _dir_end;
    const snapshot_segment *dir() const
	{ return _dir_image ? _dir_image : _dir_copy.data(); }
    // find(): a binary search of the table when it was written sorted by
    // seg_id (_dir_sorted); else the seg_id -> index map built at open() —
    // never a scan (a per-unit load sweep issues four finds per unit).
    bool _dir_sorted = false;
    std::unordered_map<uint32_t, uint32_t> _dir_index;
    bool record_ok(const snapshot_segment &s) const;
    // The one decode core: decompress (or copy) a payload into dst, no
    // transform inversion, no allocation. Both read_segment overloads and
    // read_segment_into ride it.
    bool decode_payload(const snapshot_segment &seg, uint8_t *dst) const;
public:
    snapshot_reader() : _image(0), _image_size(0), _blob(0), _blob_size(0),
			_dir_image(0), _dir_n(0), _dir_end(0)
	{ _hdr = snapshot_header(); }

    // Validates footer magic/version/sizes, the header and the directory's
    // bounds. False = not a container / corrupt -> caller falls back; never
    // throws, never reads out of bounds. A directory record is checked when
    // handed out (find / segment_at): a corrupt one is never returned.
    bool open(const void *image, size_t image_len);

    // Placement-2 profile stacks append more than one complete blob to the
    // same carrier. After open(), return the image-prefix length whose EOF is
    // the preceding valid blob's footer (skipping append_file's 0..15 bytes
    // of alignment padding). False when this is the first/only blob. The
    // caller opens that prefix with another snapshot_reader; content meaning
    // and profile selection remain the consumer's concern.
    bool previous_image_len(size_t &image_len) const;

    uint32_t segment_count() const { return _dir_n; }
    uint64_t context_hash()  const { return _hdr.context_hash; }
    // Stable identity of the OPENED BLOB within its carrier (profile stacks
    // append several blobs to one image, so the image base alone does not
    // identify a directory's segments; blob base + segment offset does).
    // For cache keys only — never dereference through it.
    const void *blob_base() const { return _blob; }
    // Where the opened blob starts within its carrier image.
    size_t blob_offset() const { return (size_t)(_blob - _image); }
    // A segment's STORED bytes (seg.comp_size of them, encoded as recorded).
    const uint8_t *stored_payload(const snapshot_segment &seg) const
	{ return _blob + seg.offset; }
    // NULL past the table or for a corrupt record.
    const snapshot_segment *segment_at(uint32_t idx) const;
    const snapshot_segment *find(uint32_t seg_id) const;

    // Decompress (or copy) a segment payload into out (resized to raw_size),
    // inverting the segment's transform when one is recorded in seg.flags.
    // The decode_bytes overload skips the resize's zero-fill (the decode
    // overwrites every byte) — prefer it for hot-path decode destinations.
    bool read_segment(const snapshot_segment &seg, std::vector<uint8_t> &out) const;
    bool read_segment(const snapshot_segment &seg, decode_bytes &out) const;

    // Same, into caller-owned storage of at least seg.raw_size bytes — no
    // allocation and no staging buffer, so a consumer that already owns the
    // destination (a typed vector sized from the directory) pays ONE
    // zero-fill instead of two plus a copy.
    bool read_segment_into(const snapshot_segment &seg,
			   uint8_t *dst, size_t capacity) const;

    // Decompress (or copy) a segment payload while preserving its recorded
    // byte-stream transform. Consumers that can bind the transformed shape
    // directly use this to avoid eagerly rebuilding the original layout.
    bool read_segment_transformed(const snapshot_segment &seg,
				  std::vector<uint8_t> &out) const;
    bool read_segment_transformed(const snapshot_segment &seg,
				  decode_bytes &out) const;

    // Zero-copy payload pointer for codec None segments (bind-in-place path);
    // NULL for compressed or transformed segments.
    const uint8_t *raw_ptr(const snapshot_segment &seg) const;

    // A lazy reader over a SNAP_XFORM_BLOCKED segment (caller owns it); NULL
    // when the segment is not blocked or its block table is malformed.
    snapshot_lazy_segment *open_lazy(const snapshot_segment &seg) const;

    // --show-stats observability (accumulated across the reader's lifetime;
    // mutable because read_segment is const). The zstd trio counts real codec
    // frames only; the copy pair counts the codec-None memcpy path. raw_ptr
    // binds are zero-copy and uncounted.
    mutable unsigned long long stat_zstd_frames = 0;   // compressed payloads decoded
    mutable unsigned long long stat_zstd_bytes_out = 0; // raw bytes those produced
    mutable double             stat_zstd_secs = 0.0;    // wall seconds in the codec
    mutable unsigned long long stat_copy_calls = 0;    // codec-None payload copies
    mutable unsigned long long stat_copy_bytes = 0;
};

} // namespace dis
} // namespace madc

#endif // __MADCDIS_SNAPSHOT_H
