// madcdis_snapshot.cpp — the madc::dis pool-snapshot container (writer/reader).
// Format + placement contract in include/madcdis/snapshot.h. Compression rides
// the one in-tree implementation (madc_pch::compress/decompress, pch.cpp).

#include <algorithm>	// sorted directory: stable_sort at write, lower_bound at read
#include <cstdio>
#include <cstring>
#include <utility>
#include <sys/time.h>

#ifdef __SSE2__
#include <emmintrin.h>
#endif

#include "madcdis/snapshot.h"

namespace madc {
namespace dis {

static const char SNAP_MAGIC[8] = { 'M','A','D','C','S','N','A','P' };

static size_t align16(size_t n) { return (n + 15) & ~(size_t)15; }

// --- segment transforms (v2: snapshot_transform in segment `flags`) ---

// A transform must be valid for the payload it is attached to; open() and
// add_segment() both enforce this so the apply/invert paths can trust it.
static bool xform_valid(uint32_t flags, uint64_t raw_size)
{
	if ( flags == 0 )
		return true;
	if ( flags >> 24 )				// reserved bits
		return false;
	switch ( snap_xform_id(flags) )
	{
	case SNAP_XFORM_DELTA32:
		return snap_xform_param(flags) == 0 && raw_size % 4 == 0;
	case SNAP_XFORM_BYTEPLANE:
	{
		uint32_t stride = snap_xform_param(flags);
		return stride > 0 && raw_size % stride == 0;
	}
	case SNAP_XFORM_BLOCKED:
	{
		uint32_t shift = snap_xform_param(flags);
		return shift >= 10 && shift <= 24;
	}
	default:
		return false;
	}
}

// --- SNAP_XFORM_BLOCKED: container-level framing, not a byte recoding ---

static size_t block_raw_len(uint64_t raw_size, uint32_t shift, size_t b)
{
	uint64_t off = (uint64_t)b << shift;
	uint64_t bs = (uint64_t)1 << shift;
	return (size_t)(raw_size - off < bs ? raw_size - off : bs);
}

static bool block_at(const uint8_t *payload, size_t b, snapshot_block &e)
{
	memcpy(&e, payload + sizeof(snapshot_block_header) + b * sizeof(snapshot_block),
	       sizeof e);
	return true;
}

// Writer: the block header, the table, then each block — one codec frame, or
// raw when it is under SNAP_BLOCK_RAW_FLOOR bytes or the codec did not shrink
// it (so stored == raw identifies a raw block unambiguously).
static bool blocked_encode(const uint8_t *raw, size_t len, uint32_t shift,
			   PchCompression codec, int level,
			   std::vector<uint8_t> &out)
{
	size_t bs = (size_t)1 << shift;
	uint64_t n = ((uint64_t)len + bs - 1) >> shift;
	if ( n > 0xffffffffu )
		return false;
	snapshot_block_header h;
	h.block_shift = shift;
	h.block_count = (uint32_t)n;
	out.assign(sizeof h + (size_t)n * sizeof(snapshot_block), 0);
	memcpy(out.data(), &h, sizeof h);
	std::vector<uint8_t> in, frame;
	for ( size_t b = 0; b < (size_t)n; ++b )
	{
		size_t off = b << shift;
		size_t blen = block_raw_len(len, shift, b);
		const uint8_t *src = raw + off;
		size_t slen = blen;
		if ( blen >= SNAP_BLOCK_RAW_FLOOR && codec != PchCompression::None )
		{
			in.assign(raw + off, raw + off + blen);
			if ( !madc_pch::compress(in, frame, codec, level) )
				return false;
			if ( frame.size() < blen )
			{
				src = frame.data();
				slen = frame.size();
			}
		}
		if ( (uint64_t)out.size() + slen > 0xffffffffu )
			return false;
		snapshot_block e;
		e.offset = (uint32_t)out.size();
		e.stored = (uint32_t)slen;
		memcpy(out.data() + sizeof h + b * sizeof e, &e, sizeof e);
		out.insert(out.end(), src, src + slen);
	}
	return true;
}

// The block table checked against the segment record: the header's shift is
// the flags', the count covers raw_size, every block lies inside the stored
// payload after the table, and none stores more than its raw length.
static bool blocked_table_valid(const uint8_t *payload, uint64_t stored,
				uint64_t raw_size, uint32_t flags)
{
	snapshot_block_header h;
	if ( stored < sizeof h )
		return false;
	memcpy(&h, payload, sizeof h);
	uint32_t shift = snap_xform_param(flags);
	if ( h.block_shift != shift )
		return false;
	uint64_t n = (raw_size + ((uint64_t)1 << shift) - 1) >> shift;
	if ( h.block_count != n )
		return false;
	uint64_t head = sizeof h + n * sizeof(snapshot_block);
	if ( head > stored )
		return false;
	for ( size_t b = 0; b < (size_t)n; ++b )
	{
		snapshot_block e;
		block_at(payload, b, e);
		if ( e.offset < head || (uint64_t)e.offset + e.stored > stored
		     || e.stored > block_raw_len(raw_size, shift, b) )
			return false;
	}
	return true;
}

// Decode block b of a validated BLOCKED payload into dst + (b << shift).
static bool blocked_decode(const uint8_t *payload, uint64_t raw_size,
			   uint32_t shift, uint32_t codec, size_t b, uint8_t *dst,
			   bool &framed)
{
	snapshot_block e;
	block_at(payload, b, e);
	size_t blen = block_raw_len(raw_size, shift, b);
	uint8_t *out = dst + ((size_t)b << shift);
	framed = e.stored != blen;
	if ( !framed )
	{
		memcpy(out, payload + e.offset, blen);
		return true;
	}
	return madc_pch::decompress(payload + e.offset, e.stored, out, blen,
				    (PchCompression)codec);
}

static void delta32_fwd(uint8_t *buf, size_t len)
{
	uint32_t prev = 0;
	for ( size_t i = 0; i + 4 <= len; i += 4 )
	{
		uint32_t cur;
		memcpy(&cur, buf + i, 4);
		uint32_t d = cur - prev;		// lossless mod 2^32
		memcpy(buf + i, &d, 4);
		prev = cur;
	}
}

static void delta32_inv(uint8_t *buf, size_t len)
{
	uint32_t acc = 0;
	for ( size_t i = 0; i + 4 <= len; i += 4 )
	{
		uint32_t d;
		memcpy(&d, buf + i, 4);
		acc += d;
		memcpy(buf + i, &acc, 4);
	}
}

// Byte-plane transpose: record-major <-> plane-major. Both directions are the
// one byte-matrix transpose out[c*rows + r] = in[r*cols + c] — fwd with
// (rows = n records, cols = stride), inv with (rows = stride, cols = n).
// A scalar loop moves one byte per load+store (~550 MB/s — it cost bound
// compiles +110ms on the packed corpus), so the bulk goes through 16x16
// SSE2 tiles (~2.4 GB/s measured); scalar covers the sub-tile tails and
// non-SSE builds.

#ifdef __SSE2__
// Transpose the 16x16 byte tile at in (row stride in_stride) into out (row
// stride out_stride): out[c][r] = in[r][c]. Four unpack stages at doubling
// element width; after stage 2, a[4q+i] holds row-group i (rows 4i..4i+3)
// of col-quad q.
static void transpose_tile16(const uint8_t *in, size_t in_stride,
			     uint8_t *out, size_t out_stride)
{
	__m128i a[16], b[16];
	int i, q;
	for ( i = 0; i < 16; i++ )
		a[i] = _mm_loadu_si128((const __m128i *)(in + (size_t)i * in_stride));
	for ( i = 0; i < 8; i++ )
	{
		b[i]     = _mm_unpacklo_epi8(a[2*i], a[2*i+1]);	// rows(2i,2i+1) cols 0-7
		b[i + 8] = _mm_unpackhi_epi8(a[2*i], a[2*i+1]);	// cols 8-15
	}
	for ( i = 0; i < 4; i++ )
	{
		a[i]      = _mm_unpacklo_epi16(b[2*i], b[2*i+1]);	// cols 0-3
		a[4 + i]  = _mm_unpackhi_epi16(b[2*i], b[2*i+1]);	// cols 4-7
		a[8 + i]  = _mm_unpacklo_epi16(b[8 + 2*i], b[9 + 2*i]);	// cols 8-11
		a[12 + i] = _mm_unpackhi_epi16(b[8 + 2*i], b[9 + 2*i]);	// cols 12-15
	}
	for ( q = 0; q < 4; q++ )
	{
		b[4*q + 0] = _mm_unpacklo_epi32(a[4*q + 0], a[4*q + 1]);	// cols 4q,4q+1 rows 0-7
		b[4*q + 1] = _mm_unpackhi_epi32(a[4*q + 0], a[4*q + 1]);	// cols 4q+2,+3 rows 0-7
		b[4*q + 2] = _mm_unpacklo_epi32(a[4*q + 2], a[4*q + 3]);	// cols 4q,4q+1 rows 8-15
		b[4*q + 3] = _mm_unpackhi_epi32(a[4*q + 2], a[4*q + 3]);	// cols 4q+2,+3 rows 8-15
	}
	for ( q = 0; q < 4; q++ )
	{
		_mm_storeu_si128((__m128i *)(out + (size_t)(4*q + 0) * out_stride),
				 _mm_unpacklo_epi64(b[4*q + 0], b[4*q + 2]));
		_mm_storeu_si128((__m128i *)(out + (size_t)(4*q + 1) * out_stride),
				 _mm_unpackhi_epi64(b[4*q + 0], b[4*q + 2]));
		_mm_storeu_si128((__m128i *)(out + (size_t)(4*q + 2) * out_stride),
				 _mm_unpacklo_epi64(b[4*q + 1], b[4*q + 3]));
		_mm_storeu_si128((__m128i *)(out + (size_t)(4*q + 3) * out_stride),
				 _mm_unpackhi_epi64(b[4*q + 1], b[4*q + 3]));
	}
}
#endif

static void byte_transpose(const uint8_t *in, uint8_t *out, size_t rows, size_t cols)
{
	size_t rt = 0, ct = 0;
#ifdef __SSE2__
	rt = rows & ~(size_t)15;
	ct = cols & ~(size_t)15;
	// Column-major tile order: each out row (a full transposed column run)
	// is completed before moving on, so writes stream contiguously.
	for ( size_t c = 0; c < ct; c += 16 )
		for ( size_t r = 0; r < rt; r += 16 )
			transpose_tile16(in + r * cols + c, cols,
					 out + c * rows + r, rows);
#endif
	for ( size_t r = rt; r < rows; ++r )
		for ( size_t c = 0; c < cols; ++c )
			out[c * rows + r] = in[r * cols + c];
	for ( size_t c = ct; c < cols; ++c )
		for ( size_t r = 0; r < rt; ++r )
			out[c * rows + r] = in[r * cols + c];
}

static void byteplane_fwd(const uint8_t *in, uint8_t *out, size_t n, size_t stride)
{
	byte_transpose(in, out, n, stride);
}

static void byteplane_inv(const uint8_t *in, uint8_t *out, size_t n, size_t stride)
{
	byte_transpose(in, out, stride, n);
}

// Forward transform in place on the staged payload (writer side).
static bool xform_apply(uint32_t flags, std::vector<uint8_t> &buf)
{
	switch ( snap_xform_id(flags) )
	{
	case SNAP_XFORM_NONE:
		return true;
	case SNAP_XFORM_DELTA32:
		delta32_fwd(buf.data(), buf.size());
		return true;
	case SNAP_XFORM_BYTEPLANE:
	{
		size_t stride = snap_xform_param(flags);
		std::vector<uint8_t> planes(buf.size());
		byteplane_fwd(buf.data(), planes.data(), buf.size() / stride, stride);
		buf.swap(planes);
		return true;
	}
	default:
		return false;
	}
}

// --- writer ---

bool snapshot_writer::add_segment(uint32_t seg_id, uint32_t kind,
				  const void *bytes, size_t len, PchCompression codec,
				  int level, uint32_t xform)
{
    for ( size_t i = 0; i < _segs.size(); ++i )
	if ( _segs[i].meta.seg_id == seg_id )
	    return false;			// duplicate id

    if ( !xform_valid(xform, (uint64_t)len) )
	return false;

    Pending p;
    p.meta.seg_id = seg_id;
    p.meta.kind = kind;
    p.meta.offset = 0;				// assigned at build()
    p.meta.raw_size = (uint64_t)len;
    p.meta.codec = (uint32_t)codec;
    p.meta.flags = xform;

    if ( snap_xform_id(xform) == SNAP_XFORM_BLOCKED )
    {
	if ( !blocked_encode((const uint8_t *)bytes, len, snap_xform_param(xform),
			     codec, level, p.payload) )
	    return false;
	p.meta.comp_size = (uint64_t)p.payload.size();
	_segs.push_back(std::move(p));
	return true;
    }
    std::vector<uint8_t> raw((const uint8_t *)bytes, (const uint8_t *)bytes + len);
    if ( xform && !xform_apply(xform, raw) )
	return false;
    if ( codec == PchCompression::None )
	p.payload.swap(raw);
    else if ( !madc_pch::compress(raw, p.payload, codec, level) )
	return false;
    p.meta.comp_size = (uint64_t)p.payload.size();

    _segs.push_back(std::move(p));
    return true;
}

bool snapshot_writer::build(std::vector<uint8_t> &out) const
{
    snapshot_header hdr;
    memcpy(hdr.magic, SNAP_MAGIC, 8);
    hdr.version = SNAPSHOT_FORMAT_VERSION;
    hdr.pool_kind = 0;
    hdr.context_hash = _context_hash;
    hdr.segment_count = (uint32_t)_segs.size();
    hdr.header_bytes = (uint32_t)sizeof(snapshot_header);

    // Lay out: header, 16-aligned payloads, 16-aligned directory, footer.
    size_t pos = sizeof(snapshot_header);
    std::vector<snapshot_segment> dir;
    dir.reserve(_segs.size());
    for ( size_t i = 0; i < _segs.size(); ++i )
    {
	pos = align16(pos);
	snapshot_segment meta = _segs[i].meta;
	meta.offset = (uint64_t)pos;
	dir.push_back(meta);
	pos += _segs[i].payload.size();
    }
    pos = align16(pos);
    size_t dir_offset = pos;
    pos += dir.size() * sizeof(snapshot_segment);
    size_t blob_size = pos + sizeof(snapshot_footer);

    snapshot_footer ftr;
    ftr.dir_offset = (uint64_t)dir_offset;
    ftr.blob_size = (uint64_t)blob_size;
    ftr.segment_count = (uint32_t)dir.size();
    ftr.version = SNAPSHOT_FORMAT_VERSION;
    memcpy(ftr.magic, SNAP_MAGIC, 8);

    out.assign(blob_size, 0);
    memcpy(out.data(), &hdr, sizeof(hdr));
    for ( size_t i = 0; i < _segs.size(); ++i )
	if ( !_segs[i].payload.empty() )
	    memcpy(out.data() + dir[i].offset, _segs[i].payload.data(), _segs[i].payload.size());
    // The directory table is written sorted by seg_id (ids are unique —
    // add_segment refuses a duplicate), so a reader finds a segment by binary
    // search in the table it already holds — no per-open index to build.
    std::stable_sort(dir.begin(), dir.end(),
		     [](const snapshot_segment &a, const snapshot_segment &b) {
			 return a.seg_id < b.seg_id;
		     });
    if ( !dir.empty() )
	memcpy(out.data() + dir_offset, dir.data(), dir.size() * sizeof(snapshot_segment));
    memcpy(out.data() + blob_size - sizeof(ftr), &ftr, sizeof(ftr));
    return true;
}

bool snapshot_writer::write_file(const char *path) const
{
    std::vector<uint8_t> blob;
    if ( !build(blob) )
	return false;
    FILE *f = fopen(path, "wb");
    if ( !f )
	return false;
    size_t n = fwrite(blob.data(), 1, blob.size(), f);
    bool ok = ( n == blob.size() ) && ( fclose(f) == 0 );
    if ( n != blob.size() )
	fclose(f);
    return ok;
}

bool snapshot_append_blob(const char *path, const void *blob, size_t len)
{
    FILE *f = fopen(path, "ab");
    if ( !f )
	return false;
    // Pad the host file to 16 so the blob base — and with it every 16-aligned
    // payload offset — is 16-aligned in the mapped image (the bind-in-place
    // contract in snapshot.h).
    long end = 0;
    if ( fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) < 0 )
    {
	fclose(f);
	return false;
    }
    static const uint8_t zeros[16] = { 0 };
    size_t pad = (size_t)(align16((size_t)end) - (size_t)end);
    if ( pad && fwrite(zeros, 1, pad, f) != pad )
    {
	fclose(f);
	return false;
    }
    size_t n = fwrite(blob, 1, len, f);
    bool ok = ( n == len ) && ( fclose(f) == 0 );
    if ( n != len )
	fclose(f);
    return ok;
}

bool snapshot_writer::append_file(const char *path) const
{
    std::vector<uint8_t> blob;
    if ( !build(blob) )
	return false;
    return snapshot_append_blob(path, blob.data(), blob.size());
}

// --- reader ---

bool snapshot_reader::open(const void *image, size_t image_len)
{
    _image = 0;
    _image_size = 0;
    _blob = 0;
    _blob_size = 0;
    _dir.clear();

    if ( !image || image_len < sizeof(snapshot_header) + sizeof(snapshot_footer) )
	return false;
    const uint8_t *bytes = (const uint8_t *)image;

    snapshot_footer ftr;
    memcpy(&ftr, bytes + image_len - sizeof(ftr), sizeof(ftr));
    if ( memcmp(ftr.magic, SNAP_MAGIC, 8) != 0 )
	return false;
    // v1 blobs (flags always 0) stay readable; anything newer than this
    // reader is rejected -> the consumer falls back.
    if ( ftr.version < 1 || ftr.version > SNAPSHOT_FORMAT_VERSION )
	return false;
    if ( ftr.blob_size > image_len
	 || ftr.blob_size < sizeof(snapshot_header) + sizeof(snapshot_footer) )
	return false;

    const uint8_t *blob = bytes + (image_len - (size_t)ftr.blob_size);
    memcpy(&_hdr, blob, sizeof(_hdr));
    if ( memcmp(_hdr.magic, SNAP_MAGIC, 8) != 0 )
	return false;
    if ( _hdr.version != ftr.version
	 || _hdr.header_bytes < sizeof(snapshot_header)
	 || _hdr.segment_count != ftr.segment_count )
	return false;

    uint64_t dir_bytes = (uint64_t)ftr.segment_count * sizeof(snapshot_segment);
    if ( ftr.dir_offset < _hdr.header_bytes
	 || ftr.dir_offset + dir_bytes + sizeof(snapshot_footer) > ftr.blob_size )
	return false;

    _dir.resize(ftr.segment_count);
    _dir_index.clear();
    _dir_sorted = true;
    if ( ftr.segment_count )
	memcpy(_dir.data(), blob + ftr.dir_offset, (size_t)dir_bytes);
    for ( size_t i = 0; i < _dir.size(); ++i )
    {
	const snapshot_segment &s = _dir[i];
	if ( s.offset < _hdr.header_bytes
	     || s.offset + s.comp_size > ftr.dir_offset )
	{
	    _dir.clear();
	    _dir_index.clear();
	    return false;
	}
	if ( s.codec == (uint32_t)PchCompression::None && s.comp_size != s.raw_size )
	{
	    _dir.clear();
	    _dir_index.clear();
	    return false;
	}
	// Transform vocabulary is v2; a v1 blob must carry zeroed flags, and
	// any recorded transform must fit the payload so read_segment can
	// trust it.
	if ( ( _hdr.version < 2 && s.flags != 0 )
	     || !xform_valid(s.flags, s.raw_size) )
	{
	    _dir.clear();
	    _dir_index.clear();
	    return false;
	}
	if ( i && _dir[i - 1].seg_id > s.seg_id )
	    _dir_sorted = false;
    }
    // A table written sorted (every writer since the sorted-directory
    // change) is searched in place; an older unsorted one gets the seg_id
    // index — first-wins, matching the linear find() it replaced.
    if ( !_dir_sorted )
    {
	_dir_index.reserve(_dir.size());
	for ( size_t i = 0; i < _dir.size(); ++i )
	    _dir_index.emplace(_dir[i].seg_id, (uint32_t)i);
    }

    _blob = blob;
    _blob_size = (size_t)ftr.blob_size;
    _image = bytes;
    _image_size = image_len;
    return true;
}

bool snapshot_reader::previous_image_len(size_t &image_len) const
{
    if ( !_image || !_blob || _blob < _image
      || (size_t)(_blob - _image) > _image_size )
	return false;

    const size_t blob_begin = (size_t)(_blob - _image);
    for ( size_t pad = 0; pad < 16 && pad <= blob_begin; ++pad )
    {
	const size_t candidate_len = blob_begin - pad;
	if ( candidate_len < sizeof(snapshot_header) + sizeof(snapshot_footer) )
	    continue;
	snapshot_reader candidate;
	if ( candidate.open(_image, candidate_len) )
	{
	    image_len = candidate_len;
	    return true;
	}
    }
    return false;
}

const snapshot_segment *snapshot_reader::find(uint32_t seg_id) const
{
    if ( _dir_sorted )
    {
	std::vector<snapshot_segment>::const_iterator it = std::lower_bound(
	    _dir.begin(), _dir.end(), seg_id,
	    [](const snapshot_segment &s, uint32_t id) { return s.seg_id < id; });
	return it != _dir.end() && it->seg_id == seg_id
	     ? &*it : (const snapshot_segment *)0;
    }
    std::unordered_map<uint32_t, uint32_t>::const_iterator it =
	_dir_index.find(seg_id);
    return it != _dir_index.end() ? &_dir[it->second]
				  : (const snapshot_segment *)0;
}

bool snapshot_reader::decode_payload(const snapshot_segment &seg,
				     uint8_t *dst) const
{
    const uint8_t *payload = _blob + seg.offset;
    if ( snap_xform_id(seg.flags) == SNAP_XFORM_BLOCKED )
    {
	// Blocking frames the payload; it does not recode the bytes, so the
	// "transformed" form IS the raw form — every block, in order.
	if ( !blocked_table_valid(payload, seg.comp_size, seg.raw_size, seg.flags) )
	    return false;
	uint32_t shift = snap_xform_param(seg.flags);
	size_t n = (size_t)((seg.raw_size + ((uint64_t)1 << shift) - 1) >> shift);
	for ( size_t b = 0; b < n; ++b )
	{
	    bool framed = false;
	    if ( !blocked_decode(payload, seg.raw_size, shift, seg.codec, b, dst, framed) )
		return false;
	    size_t blen = block_raw_len(seg.raw_size, shift, b);
	    if ( framed )
	    {
		stat_zstd_frames += 1;
		stat_zstd_bytes_out += blen;
	    }
	    else
	    {
		stat_copy_calls += 1;
		stat_copy_bytes += blen;
	    }
	}
	return true;
    }
    if ( seg.codec == (uint32_t)PchCompression::None )
    {
	if ( seg.raw_size )
	    memcpy(dst, payload, (size_t)seg.raw_size);
	stat_copy_calls += 1;
	stat_copy_bytes += seg.raw_size;
    }
    else
    {
	struct timeval t0, t1;
	gettimeofday(&t0, NULL);
	if ( !madc_pch::decompress(payload, (size_t)seg.comp_size,
				   dst, (size_t)seg.raw_size,
				   (PchCompression)seg.codec) )
	    return false;
	gettimeofday(&t1, NULL);
	stat_zstd_frames += 1;
	stat_zstd_bytes_out += seg.raw_size;
	stat_zstd_secs += (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    }
    return true;
}

bool snapshot_reader::read_segment_transformed(const snapshot_segment &seg,
					std::vector<uint8_t> &out) const
{
    if ( !_blob )
	return false;
    out.resize((size_t)seg.raw_size);
    return decode_payload(seg, out.data());
}

bool snapshot_reader::read_segment_transformed(const snapshot_segment &seg,
					decode_bytes &out) const
{
    if ( !_blob )
	return false;
    out.resize((size_t)seg.raw_size);	// no zero-fill (default_init_allocator)
    return decode_payload(seg, out.data());
}

bool snapshot_reader::read_segment_into(const snapshot_segment &seg,
					uint8_t *dst, size_t capacity) const
{
    if ( !_blob || capacity < (size_t)seg.raw_size )
	return false;
    if ( !seg.raw_size )
	return true;
    if ( !dst )
	return false;

    // Byte-plane inversion permutes rather than mutates, so it decodes via a
    // scratch buffer; the in-place transforms decode straight into dst. The
    // scratch is reused across calls (grow-only) — a per-call vector re-pays
    // page faults on every multi-MB segment of a lazy per-unit load sweep.
    uint32_t xid = snap_xform_id(seg.flags);
    if ( xid == SNAP_XFORM_BYTEPLANE )
    {
	static thread_local decode_bytes planes;
	if ( !read_segment_transformed(seg, planes) )
	    return false;
	size_t stride = snap_xform_param(seg.flags);
	byteplane_inv(planes.data(), dst, (size_t)seg.raw_size / stride, stride);
	return true;
    }

    if ( !decode_payload(seg, dst) )
	return false;
    if ( xid == SNAP_XFORM_DELTA32 )
	delta32_inv(dst, (size_t)seg.raw_size);
    return true;
}

bool snapshot_reader::read_segment(const snapshot_segment &seg, std::vector<uint8_t> &out) const
{
    out.resize((size_t)seg.raw_size);
    return read_segment_into(seg, out.data(), out.size());
}

bool snapshot_reader::read_segment(const snapshot_segment &seg, decode_bytes &out) const
{
    out.resize((size_t)seg.raw_size);	// no zero-fill (default_init_allocator)
    return read_segment_into(seg, out.data(), out.size());
}

const uint8_t *snapshot_reader::raw_ptr(const snapshot_segment &seg) const
{
    if ( !_blob || seg.codec != (uint32_t)PchCompression::None || seg.flags )
	return (const uint8_t *)0;
    return _blob + seg.offset;
}

snapshot_lazy_segment *snapshot_reader::open_lazy(const snapshot_segment &seg) const
{
    if ( !_blob || snap_xform_id(seg.flags) != SNAP_XFORM_BLOCKED
	 || !blocked_table_valid(_blob + seg.offset, seg.comp_size,
				 seg.raw_size, seg.flags) )
	return (snapshot_lazy_segment *)0;
    return new snapshot_lazy_segment(_blob + seg.offset, seg);
}

// --- snapshot_lazy_segment ---

snapshot_lazy_segment::snapshot_lazy_segment(const uint8_t *payload,
					     const snapshot_segment &seg)
    : _payload(payload), _codec(seg.codec), _seg_id(seg.seg_id)
{
    _size = (size_t)seg.raw_size;
    _shift = snap_xform_param(seg.flags);
    _nblocks = (size_t)((seg.raw_size + ((uint64_t)1 << _shift) - 1) >> _shift);
    // Uninitialized: a block's bytes are written by its decode, and pages of
    // blocks never touched are never faulted in.
    _buf = _size ? new uint8_t[_size] : (uint8_t *)0;
    _ready = _nblocks ? new std::atomic<uint8_t>[_nblocks] : (std::atomic<uint8_t> *)0;
    for ( size_t b = 0; b < _nblocks; ++b )
	_ready[b].store(0, std::memory_order_relaxed);
}

snapshot_lazy_segment::~snapshot_lazy_segment()
{
    delete[] _buf;
    delete[] _ready;
}

bool snapshot_lazy_segment::fill(size_t first, size_t last) const
{
    std::lock_guard<std::mutex> lk(_mu);
    for ( size_t b = first; b <= last; ++b )
    {
	if ( _ready[b].load(std::memory_order_relaxed) )
	    continue;
	bool framed = false;
	if ( !blocked_decode(_payload, _size, _shift, _codec, b, _buf, framed) )
	{
	    fprintf(stderr, "madc: container segment %u block %zu failed to decode\n",
		    _seg_id, b);
	    return false;
	}
	_decoded.fetch_add(1, std::memory_order_relaxed);
	_ready[b].store(1, std::memory_order_release);
    }
    return true;
}

} // namespace dis
} // namespace madc
