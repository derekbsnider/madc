// madcdis_snapshot.cpp — the madc::dis pool-snapshot container (writer/reader).
// Format + placement contract in include/madcdis/snapshot.h. Compression rides
// the one in-tree implementation (madc_pch::compress/decompress, pch.cpp).

#include <cstdio>
#include <cstring>
#include <utility>

#include "madcdis/snapshot.h"

namespace madc {
namespace dis {

static const char SNAP_MAGIC[8] = { 'M','A','D','C','S','N','A','P' };

static size_t align16(size_t n) { return (n + 15) & ~(size_t)15; }

// --- writer ---

bool snapshot_writer::add_segment(uint32_t seg_id, uint32_t kind,
				  const void *bytes, size_t len, PchCompression codec)
{
    for ( size_t i = 0; i < _segs.size(); ++i )
	if ( _segs[i].meta.seg_id == seg_id )
	    return false;			// duplicate id

    Pending p;
    p.meta.seg_id = seg_id;
    p.meta.kind = kind;
    p.meta.offset = 0;				// assigned at build()
    p.meta.raw_size = (uint64_t)len;
    p.meta.codec = (uint32_t)codec;
    p.meta.flags = 0;

    std::vector<uint8_t> raw((const uint8_t *)bytes, (const uint8_t *)bytes + len);
    if ( codec == PchCompression::None )
	p.payload.swap(raw);
    else if ( !madc_pch::compress(raw, p.payload, codec) )
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

bool snapshot_writer::append_file(const char *path) const
{
    std::vector<uint8_t> blob;
    if ( !build(blob) )
	return false;
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
    size_t n = fwrite(blob.data(), 1, blob.size(), f);
    bool ok = ( n == blob.size() ) && ( fclose(f) == 0 );
    if ( n != blob.size() )
	fclose(f);
    return ok;
}

// --- reader ---

bool snapshot_reader::open(const void *image, size_t image_len)
{
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
    if ( ftr.version != SNAPSHOT_FORMAT_VERSION )
	return false;
    if ( ftr.blob_size > image_len
	 || ftr.blob_size < sizeof(snapshot_header) + sizeof(snapshot_footer) )
	return false;

    const uint8_t *blob = bytes + (image_len - (size_t)ftr.blob_size);
    memcpy(&_hdr, blob, sizeof(_hdr));
    if ( memcmp(_hdr.magic, SNAP_MAGIC, 8) != 0 )
	return false;
    if ( _hdr.version != SNAPSHOT_FORMAT_VERSION
	 || _hdr.header_bytes < sizeof(snapshot_header)
	 || _hdr.segment_count != ftr.segment_count )
	return false;

    uint64_t dir_bytes = (uint64_t)ftr.segment_count * sizeof(snapshot_segment);
    if ( ftr.dir_offset < _hdr.header_bytes
	 || ftr.dir_offset + dir_bytes + sizeof(snapshot_footer) > ftr.blob_size )
	return false;

    _dir.resize(ftr.segment_count);
    if ( ftr.segment_count )
	memcpy(_dir.data(), blob + ftr.dir_offset, (size_t)dir_bytes);
    for ( size_t i = 0; i < _dir.size(); ++i )
    {
	const snapshot_segment &s = _dir[i];
	if ( s.offset < _hdr.header_bytes
	     || s.offset + s.comp_size > ftr.dir_offset )
	{
	    _dir.clear();
	    return false;
	}
	if ( s.codec == (uint32_t)PchCompression::None && s.comp_size != s.raw_size )
	{
	    _dir.clear();
	    return false;
	}
    }

    _blob = blob;
    _blob_size = (size_t)ftr.blob_size;
    return true;
}

const snapshot_segment *snapshot_reader::find(uint32_t seg_id) const
{
    for ( size_t i = 0; i < _dir.size(); ++i )
	if ( _dir[i].seg_id == seg_id )
	    return &_dir[i];
    return (const snapshot_segment *)0;
}

bool snapshot_reader::read_segment(const snapshot_segment &seg, std::vector<uint8_t> &out) const
{
    if ( !_blob )
	return false;
    const uint8_t *payload = _blob + seg.offset;
    out.resize((size_t)seg.raw_size);
    if ( seg.codec == (uint32_t)PchCompression::None )
    {
	if ( seg.raw_size )
	    memcpy(out.data(), payload, (size_t)seg.raw_size);
	return true;
    }
    return madc_pch::decompress(payload, (size_t)seg.comp_size,
				 out.data(), (size_t)seg.raw_size,
				 (PchCompression)seg.codec);
}

const uint8_t *snapshot_reader::raw_ptr(const snapshot_segment &seg) const
{
    if ( !_blob || seg.codec != (uint32_t)PchCompression::None )
	return (const uint8_t *)0;
    return _blob + seg.offset;
}

} // namespace dis
} // namespace madc
