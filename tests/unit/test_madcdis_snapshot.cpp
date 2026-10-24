// Unit tests for the madc::dis pool-snapshot container (madcdis/snapshot.h)
// and the frozen intern-table tier (madcdis/intern_table.h): writer/reader
// round-trip, both placements (standalone file / appended-to-binary), codec
// paths, corruption rejection, and frozen_intern_table bind parity with the
// live table it was serialized from (bytes + the offsets block).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "madcdis/intern_table.h"
#include "madcdis/snapshot.h"

using madc::dis::intern_table;
using madc::dis::frozen_intern_table;
using madc::dis::snapshot_writer;
using madc::dis::snapshot_reader;
using madc::dis::snapshot_segment;

static std::string tmp_path(const char *tag)
{
    return std::string("/tmp/madc_snap_") + tag + "_"
	 + std::to_string((long long)getpid()) + ".bin";
}

static bool slurp(const std::string &path, std::vector<uint8_t> &out)
{
    std::ifstream is(path.c_str(), std::ios::binary);
    if ( !is )
	return false;
    out.assign(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
    return true;
}

// Build a live table with a deterministic population and remember the ids.
static void populate(intern_table &p, std::vector<uint32_t> &ids, int n)
{
    char buf[32];
    for ( int i = 0; i < n; ++i )
    {
	snprintf(buf, sizeof buf, "spelling_%d", i);
	ids.push_back(p.intern(buf));
    }
}

// Serialize a live table's two frozen blocks into a writer (the forest's
// shape): the records, and the offsets block.
static bool add_intern_segments(snapshot_writer &w, const intern_table &p,
				PchCompression codec)
{
    std::vector<char> bytes;
    std::vector<uint32_t> offs;
    p.frozen_blocks(bytes, offs);
    if ( !w.add_segment(1, madc::dis::SNAP_KIND_INTERN_BYTES,
			bytes.data(), bytes.size(), codec) )
	return false;
    return w.add_segment(2, madc::dis::SNAP_KIND_INTERN_OFFSETS,
			 offs.data(), offs.size() * sizeof(uint32_t), codec);
}

// Bind a frozen view over two decompressed block buffers.
static void bind_frozen(frozen_intern_table &fz,
			const std::vector<uint8_t> &bytes,
			const std::vector<uint8_t> &offsets)
{
    fz.bind((const char *)bytes.data(), bytes.size(),
	    (const uint32_t *)offsets.data(), offsets.size() / sizeof(uint32_t));
}

TEST_CASE("frozen view over a live table's blocks: ids and spellings")
{
    intern_table p;
    std::vector<uint32_t> ids;
    populate(p, ids, 3000);   // enough to force growth + rehash

    std::vector<char> bytes;
    std::vector<uint32_t> offs;
    p.frozen_blocks(bytes, offs);
    CHECK(offs.size() == p.count() + 2);
    CHECK(offs.back() == bytes.size());

    frozen_intern_table fz;
    fz.bind(bytes.data(), bytes.size(), offs.data(), offs.size());
    CHECK(fz.valid());
    CHECK(fz.count() == p.count());
    CHECK(fz.length(0) == 0u);

    char buf[32];
    for ( int i = 0; i < 3000; ++i )
    {
	uint32_t id = ids[(size_t)i];
	snprintf(buf, sizeof buf, "spelling_%d", i);
	CHECK(fz.str(id) == buf);
	CHECK(fz.length(id) == p.length(id));
    }
    // An id past the table reads as the empty spelling, never out of bounds.
    CHECK(std::string(fz.c_str((uint32_t)p.count() + 1)) == "");
    CHECK(fz.length((uint32_t)p.count() + 1) == 0u);

    // A closing offset that disagrees with the byte block is refused.
    offs.back() += 1;
    frozen_intern_table bad;
    bad.bind(bytes.data(), bytes.size(), offs.data(), offs.size());
    CHECK_FALSE(bad.valid());
}

TEST_CASE("frozen records carry their length: any order, embedded NULs, long spellings")
{
    intern_table p;
    std::vector<uint32_t> ids;
    populate(p, ids, 400);
    const char lit[] = { 'a', '\0', 'b', '\0' };	// a string literal's value
    const uint32_t lit_id = p.intern(lit, sizeof lit, intern_table::hash_bytes(lit, sizeof lit));
    std::string big(300, 'x');				// a two-byte LEB128 length
    const uint32_t big_id = p.intern(big.c_str());

    std::vector<char> bytes;
    std::vector<uint32_t> offs;
    p.frozen_blocks(bytes, offs);
    // Re-lay the records in reverse id order (id 0 stays at byte 0), the way
    // the pack's layout pass reorders them: only the offsets block moves.
    std::vector<char> rev(bytes.begin(), bytes.begin() + offs[1]);
    std::vector<uint32_t> roffs(offs.size());
    roffs[0] = 0;
    for ( size_t id = offs.size() - 2; id >= 1; --id )
    {
	roffs[id] = (uint32_t)rev.size();
	rev.insert(rev.end(), bytes.begin() + offs[id], bytes.begin() + offs[id + 1]);
    }
    roffs.back() = (uint32_t)rev.size();
    CHECK(rev.size() == bytes.size());

    frozen_intern_table fz;
    fz.bind(rev.data(), rev.size(), roffs.data(), roffs.size());
    CHECK(fz.valid());
    uint32_t len = 99;
    const char *sp = fz.spelling(lit_id, len);
    REQUIRE(sp != nullptr);
    CHECK(len == sizeof lit);
    CHECK(memcmp(sp, lit, sizeof lit) == 0);
    CHECK(fz.str(big_id) == big);
    CHECK(fz.length(0) == 0u);
    char buf[32];
    for ( int i = 0; i < 400; ++i )
    {
	snprintf(buf, sizeof buf, "spelling_%d", i);
	CHECK(fz.str(ids[(size_t)i]) == buf);
    }
    // A record whose length runs past the bytes reads as absent.
    rev[roffs[big_id]] = (char)0x7f;	// claims 127 bytes, then no NUL there
    CHECK(fz.str(big_id).empty());
}

TEST_CASE("container round-trip in memory (zlib) rebinds an identical frozen table")
{
    intern_table p;
    std::vector<uint32_t> ids;
    populate(p, ids, 500);

    snapshot_writer w;
    w.set_context_hash(0x1122334455667788ull);
    REQUIRE(add_intern_segments(w, p, PchCompression::Zlib));
    std::vector<uint8_t> blob;
    REQUIRE(w.build(blob));

    snapshot_reader r;
    REQUIRE(r.open(blob.data(), blob.size()));
    CHECK(r.segment_count() == 2u);
    CHECK(r.context_hash() == 0x1122334455667788ull);

    std::vector<uint8_t> bytes, offsets;
    REQUIRE(r.find(1) != nullptr);
    REQUIRE(r.find(2) != nullptr);
    CHECK(r.find(1)->kind == (uint32_t)madc::dis::SNAP_KIND_INTERN_BYTES);
    CHECK(r.find(2)->kind == (uint32_t)madc::dis::SNAP_KIND_INTERN_OFFSETS);
    REQUIRE(r.read_segment(*r.find(1), bytes));
    REQUIRE(r.read_segment(*r.find(2), offsets));
    CHECK(r.raw_ptr(*r.find(1)) == nullptr);   // compressed -> no bind-in-place

    frozen_intern_table fz;
    bind_frozen(fz, bytes, offsets);
    CHECK(fz.valid());
    CHECK(fz.count() == p.count());
    for ( size_t i = 0; i < ids.size(); ++i )
	CHECK(fz.str(ids[i]) == p.str(ids[i]));
}

TEST_CASE("codec None segments bind in place via raw_ptr")
{
    intern_table p;
    std::vector<uint32_t> ids;
    populate(p, ids, 200);

    snapshot_writer w;
    REQUIRE(add_intern_segments(w, p, PchCompression::None));
    std::vector<uint8_t> blob;
    REQUIRE(w.build(blob));

    snapshot_reader r;
    REQUIRE(r.open(blob.data(), blob.size()));
    const snapshot_segment *sb = r.find(1);
    const snapshot_segment *so = r.find(2);
    REQUIRE(sb != nullptr);
    REQUIRE(so != nullptr);
    CHECK(sb->comp_size == sb->raw_size);
    CHECK((so->offset & 15u) == 0u);   // 16-aligned payloads (bind-in-place contract)

    frozen_intern_table fz;
    fz.bind((const char *)r.raw_ptr(*sb), (size_t)sb->raw_size,
	    (const uint32_t *)r.raw_ptr(*so),
	    (size_t)so->raw_size / sizeof(uint32_t));
    CHECK(fz.valid());
    for ( size_t i = 0; i < ids.size(); ++i )
	CHECK(fz.str(ids[i]) == p.str(ids[i]));
}

TEST_CASE("a segment the codec does not shrink is stored raw and binds in place")
{
    using madc::dis::snap_xform_flags;
    using madc::dis::SNAP_XFORM_DELTA32;

    // Incompressible bytes (an LCG's high bytes): the codec's frame is larger
    // than the payload, so the writer keeps the payload's own bytes, as a
    // BLOCKED segment does per block. A compressible payload stays framed.
    std::vector<uint8_t> noise(600);
    uint32_t x = 12345u;
    for ( size_t i = 0; i < noise.size(); ++i )
    {
	x = x * 1103515245u + 12345u;
	noise[i] = (uint8_t)(x >> 24);
    }
    std::vector<uint8_t> runs(600, 7);

    snapshot_writer w;
    REQUIRE(w.add_segment(1, 0, noise.data(), noise.size(), PchCompression::Zlib));
    REQUIRE(w.add_segment(2, 0, noise.data(), noise.size(), PchCompression::Zlib,
			  0, snap_xform_flags(SNAP_XFORM_DELTA32)));
    REQUIRE(w.add_segment(3, 0, runs.data(), runs.size(), PchCompression::Zlib));
    std::vector<uint8_t> blob;
    REQUIRE(w.build(blob));

    snapshot_reader r;
    REQUIRE(r.open(blob.data(), blob.size()));
    for ( uint32_t id = 1; id <= 2; ++id )
    {
	const snapshot_segment *s = r.find(id);
	REQUIRE(s != nullptr);
	CHECK(s->codec == (uint32_t)PchCompression::None);
	CHECK(s->flags == 0u);			// its own bytes, untransformed
	CHECK(s->comp_size == s->raw_size);
	REQUIRE(r.raw_ptr(*s) != nullptr);
	CHECK(memcmp(r.raw_ptr(*s), noise.data(), noise.size()) == 0);
	std::vector<uint8_t> got;
	REQUIRE(r.read_segment(*s, got));
	CHECK(got == noise);
    }
    const snapshot_segment *framed = r.find(3);
    REQUIRE(framed != nullptr);
    CHECK(framed->codec == (uint32_t)PchCompression::Zlib);
    CHECK(framed->comp_size < framed->raw_size);
}

TEST_CASE("the directory table is sorted by seg_id; find binary-searches it")
{
    // Added out of id order: the reader finds every id in the table it holds
    // (no per-open index). A duplicate id is refused at add.
    const uint32_t first = 0x11111111u, second = 0x22222222u;
    const uint32_t a = 7, c = 9;
    snapshot_writer w;
    REQUIRE(w.add_segment(7, madc::dis::SNAP_KIND_CONSUMER, &a, sizeof(a), PchCompression::None));
    REQUIRE(w.add_segment(2, madc::dis::SNAP_KIND_CONSUMER, &first, sizeof(first), PchCompression::None));
    REQUIRE(w.add_segment(9, madc::dis::SNAP_KIND_CONSUMER, &c, sizeof(c), PchCompression::None));
    CHECK_FALSE(w.add_segment(2, madc::dis::SNAP_KIND_CONSUMER, &second, sizeof(second), PchCompression::None));
    std::vector<uint8_t> blob;
    REQUIRE(w.build(blob));

    snapshot_reader r;
    REQUIRE(r.open(blob.data(), blob.size()));
    REQUIRE(r.segment_count() == 3u);
    for ( uint32_t i = 1; i < r.segment_count(); ++i )
	CHECK(r.segment_at(i - 1)->seg_id <= r.segment_at(i)->seg_id);
    uint32_t v = 0;
    const snapshot_segment *s7 = r.find(7);
    REQUIRE(s7 != nullptr);
    memcpy(&v, r.raw_ptr(*s7), sizeof(v));
    CHECK(v == a);
    const snapshot_segment *s2 = r.find(2);
    REQUIRE(s2 != nullptr);
    memcpy(&v, r.raw_ptr(*s2), sizeof(v));
    CHECK(v == first);
    const snapshot_segment *s9 = r.find(9);
    REQUIRE(s9 != nullptr);
    memcpy(&v, r.raw_ptr(*s9), sizeof(v));
    CHECK(v == c);
    CHECK(r.find(1) == nullptr);
    CHECK(r.find(8) == nullptr);
    CHECK(r.find(10) == nullptr);
}

TEST_CASE("placement 1: standalone snapshot file round-trips")
{
    intern_table p;
    std::vector<uint32_t> ids;
    populate(p, ids, 100);

    snapshot_writer w;
    REQUIRE(add_intern_segments(w, p, PchCompression::Zlib));
    const std::string path = tmp_path("file");
    std::remove(path.c_str());
    REQUIRE(w.write_file(path.c_str()));

    std::vector<uint8_t> image;
    REQUIRE(slurp(path, image));
    snapshot_reader r;
    CHECK(r.open(image.data(), image.size()));
    CHECK(r.segment_count() == 2u);
    std::remove(path.c_str());
}

TEST_CASE("placement 2: blob appended to a host binary is found from EOF")
{
    intern_table p;
    std::vector<uint32_t> ids;
    populate(p, ids, 100);

    // Fake host binary with a deliberately 16-UNALIGNED length.
    const std::string path = tmp_path("appended");
    std::remove(path.c_str());
    {
	std::ofstream os(path.c_str(), std::ios::binary);
	std::vector<char> junk(4099, 'E');   // not a multiple of 16
	os.write(junk.data(), (std::streamsize)junk.size());
    }

    snapshot_writer w;
    REQUIRE(add_intern_segments(w, p, PchCompression::None));
    REQUIRE(w.append_file(path.c_str()));

    std::vector<uint8_t> image;
    REQUIRE(slurp(path, image));
    snapshot_reader r;
    REQUIRE(r.open(image.data(), image.size()));
    CHECK(r.segment_count() == 2u);

    // The blob base must have landed 16-aligned in the file (append pads),
    // so a None-codec payload binds in place even from the appended image.
    const snapshot_segment *se = r.find(2);
    REQUIRE(se != nullptr);
    const uint8_t *pe = r.raw_ptr(*se);
    REQUIRE(pe != nullptr);
    CHECK((((uintptr_t)pe - (uintptr_t)image.data()) & 15u) == 0u);

    std::vector<uint8_t> bytes, offsets;
    REQUIRE(r.read_segment(*r.find(1), bytes));
    REQUIRE(r.read_segment(*r.find(2), offsets));
    frozen_intern_table fz;
    bind_frozen(fz, bytes, offsets);
    CHECK(fz.valid());
    for ( size_t i = 0; i < ids.size(); ++i )
	CHECK(fz.str(ids[i]) == p.str(ids[i]));

    std::remove(path.c_str());
}

TEST_CASE("placement 2 profile stack walks valid blobs newest to oldest")
{
    const std::string path = tmp_path("stacked");
    std::remove(path.c_str());
    {
	std::ofstream os(path.c_str(), std::ios::binary);
	std::vector<char> junk(4099, 'P');
	os.write(junk.data(), (std::streamsize)junk.size());
    }

    const uint32_t older_value = 11;
    snapshot_writer older;
    older.set_context_hash(0x1111u);
    REQUIRE(older.add_segment(11, madc::dis::SNAP_KIND_RAW,
			      &older_value, sizeof(older_value),
			      PchCompression::None));
    REQUIRE(older.append_file(path.c_str()));

    const uint32_t newer_value = 22;
    snapshot_writer newer;
    newer.set_context_hash(0x2222u);
    REQUIRE(newer.add_segment(22, madc::dis::SNAP_KIND_RAW,
			      &newer_value, sizeof(newer_value),
			      PchCompression::None));
    REQUIRE(newer.append_file(path.c_str()));

    std::vector<uint8_t> image;
    REQUIRE(slurp(path, image));
    snapshot_reader latest;
    REQUIRE(latest.open(image.data(), image.size()));
    CHECK(latest.context_hash() == 0x2222u);
    CHECK(latest.find(22) != nullptr);

    size_t previous_len = 0;
    REQUIRE(latest.previous_image_len(previous_len));
    CHECK(previous_len < image.size());
    snapshot_reader previous;
    REQUIRE(previous.open(image.data(), previous_len));
    CHECK(previous.context_hash() == 0x1111u);
    CHECK(previous.find(11) != nullptr);
    CHECK_FALSE(previous.previous_image_len(previous_len));

    std::remove(path.c_str());
}

// One small container (one RAW u32 segment under `hash`), built in memory.
static std::vector<uint8_t> small_blob(uint32_t hash, uint32_t value)
{
    snapshot_writer w;
    w.set_context_hash(hash);
    std::vector<uint8_t> blob;
    if ( !w.add_segment(value, madc::dis::SNAP_KIND_RAW, &value, sizeof(value),
			PchCompression::None) || !w.build(blob) )
	blob.clear();
    return blob;
}

// Zero-pad `img` to a multiple of `align`, as a writer pads ahead of a blob.
static void pad_to(std::vector<uint8_t> &img, size_t align)
{
    img.resize((img.size() + align - 1) / align * align, 0);
}

static size_t nonzero_between(const std::vector<uint8_t> &img, size_t from, size_t to)
{
    size_t n = 0;
    for ( size_t i = from; i < to; ++i )
	n += img[i] != 0;
    return n;
}

TEST_CASE("placement 2: every appended container starts on a 4096-byte page")
{
    const size_t page = 4096;
    const std::string path = tmp_path("paged");
    std::remove(path.c_str());
    {
	std::ofstream os(path.c_str(), std::ios::binary);
	std::vector<char> junk(4099, 'Q');	// odd: no alignment at all
	os.write(junk.data(), (std::streamsize)junk.size());
    }
    const uint32_t first_value = 31, second_value = 32;
    snapshot_writer first;
    first.set_context_hash(0xa1u);
    REQUIRE(first.add_segment(31, madc::dis::SNAP_KIND_RAW, &first_value,
			      sizeof(first_value), PchCompression::None));
    REQUIRE(first.append_file(path.c_str()));
    snapshot_writer second;
    second.set_context_hash(0xa2u);
    REQUIRE(second.add_segment(32, madc::dis::SNAP_KIND_RAW, &second_value,
			       sizeof(second_value), PchCompression::None));
    REQUIRE(second.append_file(path.c_str()));

    std::vector<uint8_t> image;
    REQUIRE(slurp(path, image));
    snapshot_reader latest;
    REQUIRE(latest.open(image.data(), image.size()));
    CHECK(latest.context_hash() == 0xa2u);
    CHECK(latest.blob_offset() % page == 0u);

    size_t below = 0;
    REQUIRE(latest.previous_image_len(below));
    snapshot_reader previous;
    REQUIRE(previous.open(image.data(), below));
    CHECK(previous.context_hash() == 0xa1u);
    CHECK(previous.find(31) != nullptr);
    CHECK(previous.blob_offset() % page == 0u);
    // The host's bytes end where its pad begins; the pad is zeros.
    CHECK(previous.blob_offset() >= 4099u);
    CHECK(nonzero_between(image, 4099, previous.blob_offset()) == 0u);
    CHECK(nonzero_between(image, below, latest.blob_offset()) == 0u);
    CHECK_FALSE(previous.previous_image_len(below));

    std::remove(path.c_str());
}

TEST_CASE("placement 2: the profile walk crosses any zero pad below a page, and no further")
{
    const size_t page = 4096;
    const std::vector<uint8_t> older = small_blob(0xb1u, 41);
    const std::vector<uint8_t> newer = small_blob(0xb2u, 42);
    REQUIRE(!older.empty());
    REQUIRE(!newer.empty());

    // Page pads, laid by hand: the walk crosses thousands of zeros.
    {
	std::vector<uint8_t> img(37, 0xab);
	pad_to(img, page);
	const size_t older_at = img.size();
	img.insert(img.end(), older.begin(), older.end());
	pad_to(img, page);
	const size_t newer_at = img.size();
	img.insert(img.end(), newer.begin(), newer.end());
	REQUIRE(newer_at - (older_at + older.size()) > 16u);
	snapshot_reader latest;
	REQUIRE(latest.open(img.data(), img.size()));
	size_t below = 0;
	REQUIRE(latest.previous_image_len(below));
	CHECK(below == older_at + older.size());
	snapshot_reader previous;
	REQUIRE(previous.open(img.data(), below));
	CHECK(previous.context_hash() == 0xb1u);
	CHECK(previous.blob_offset() == older_at);
	CHECK_FALSE(previous.previous_image_len(below));
    }

    // An older writer's 16-byte pads still walk.
    {
	std::vector<uint8_t> img(37, 0xab);
	pad_to(img, 16);
	img.insert(img.end(), older.begin(), older.end());
	const size_t older_end = img.size();
	pad_to(img, 16);
	img.insert(img.end(), newer.begin(), newer.end());
	snapshot_reader latest;
	REQUIRE(latest.open(img.data(), img.size()));
	size_t below = 0;
	REQUIRE(latest.previous_image_len(below));
	CHECK(below == older_end);
	snapshot_reader previous;
	REQUIRE(previous.open(img.data(), below));
	CHECK(previous.context_hash() == 0xb1u);
    }

    // The bound: page - 1 zeros are a pad, a page of zeros is not.
    for ( size_t zeros = page - 1; zeros <= page; ++zeros )
    {
	std::vector<uint8_t> img(older.begin(), older.end());
	img.resize(img.size() + zeros, 0);
	img.insert(img.end(), newer.begin(), newer.end());
	snapshot_reader latest;
	REQUIRE(latest.open(img.data(), img.size()));
	size_t below = 0;
	CHECK(latest.previous_image_len(below) == (zeros < page));
    }

    // A host ending in zeros, with no container under them: none.
    {
	std::vector<uint8_t> img(100, 'E');
	img.resize(301, 0);
	pad_to(img, page);
	img.insert(img.end(), newer.begin(), newer.end());
	snapshot_reader latest;
	REQUIRE(latest.open(img.data(), img.size()));
	size_t below = 0;
	CHECK_FALSE(latest.previous_image_len(below));
    }
    // A host of zeros alone, longer than a page: none.
    {
	std::vector<uint8_t> img(5000, 0);
	pad_to(img, page);
	img.insert(img.end(), newer.begin(), newer.end());
	snapshot_reader latest;
	REQUIRE(latest.open(img.data(), img.size()));
	size_t below = 0;
	CHECK_FALSE(latest.previous_image_len(below));
    }
}

TEST_CASE("a file with no blob, or a corrupt one, is rejected cleanly")
{
    snapshot_reader r;

    // Too short / no magic.
    std::vector<uint8_t> junk(10, 0x5a);
    CHECK(!r.open(junk.data(), junk.size()));
    junk.assign(4096, 0x5a);
    CHECK(!r.open(junk.data(), junk.size()));

    // A real blob with the footer magic clobbered.
    intern_table p;
    std::vector<uint32_t> ids;
    populate(p, ids, 10);
    snapshot_writer w;
    REQUIRE(add_intern_segments(w, p, PchCompression::None));
    std::vector<uint8_t> blob;
    REQUIRE(w.build(blob));
    blob[blob.size() - 1] ^= 0xff;
    CHECK(!r.open(blob.data(), blob.size()));
    blob[blob.size() - 1] ^= 0xff;
    REQUIRE(r.open(blob.data(), blob.size()));

    // Truncated blob (payload sliced off the front is undetectable, but a
    // truncated TAIL must fail the footer/size checks).
    std::vector<uint8_t> truncated(blob.begin(), blob.end() - 8);
    CHECK(!r.open(truncated.data(), truncated.size()));

    // Duplicate seg_id is refused at write time.
    snapshot_writer w2;
    CHECK(w2.add_segment(7, 0, "abc", 3, PchCompression::None));
    CHECK(!w2.add_segment(7, 0, "def", 3, PchCompression::None));
}

TEST_CASE("empty container (zero segments) still round-trips")
{
    snapshot_writer w;
    std::vector<uint8_t> blob;
    REQUIRE(w.build(blob));
    snapshot_reader r;
    REQUIRE(r.open(blob.data(), blob.size()));
    CHECK(r.segment_count() == 0u);
    CHECK(r.find(1) == nullptr);
}

TEST_CASE("segment transforms round-trip losslessly (v2 flags vocabulary)")
{
    using madc::dis::snap_xform_flags;
    using madc::dis::SNAP_XFORM_DELTA32;
    using madc::dis::SNAP_XFORM_BYTEPLANE;

    // A u32 stream with ascending runs, a wrap-around drop, and a
    // connector-style high-bit entry — delta must be lossless mod 2^32.
    std::vector<uint32_t> idx;
    for ( uint32_t i = 0; i < 1000; ++i )
	idx.push_back(i);
    idx.push_back(3u);
    idx.push_back(0x80000005u);
    idx.push_back(7u);

    // Fixed-stride 8-byte records with columnar redundancy.
    std::vector<uint8_t> recs;
    for ( uint32_t i = 0; i < 500; ++i )
    {
	uint8_t rec[8] = { (uint8_t)i, 0, 0, 0, 42, 0, (uint8_t)(i >> 8), 0 };
	recs.insert(recs.end(), rec, rec + 8);
    }

    snapshot_writer w;
    REQUIRE(w.add_segment(1, 0, idx.data(), idx.size() * 4, PchCompression::Zlib,
			  0, snap_xform_flags(SNAP_XFORM_DELTA32)));
    REQUIRE(w.add_segment(2, 0, recs.data(), recs.size(), PchCompression::Zlib,
			  0, snap_xform_flags(SNAP_XFORM_BYTEPLANE, 8)));
    // Transform with codec None: stored bytes are transformed, so no
    // bind-in-place — but read_segment must still return the original.
    REQUIRE(w.add_segment(3, 0, idx.data(), idx.size() * 4, PchCompression::None,
			  0, snap_xform_flags(SNAP_XFORM_DELTA32)));
    std::vector<uint8_t> blob;
    REQUIRE(w.build(blob));

    snapshot_reader r;
    REQUIRE(r.open(blob.data(), blob.size()));
    std::vector<uint8_t> got;
    std::vector<uint8_t> transformed;
    REQUIRE(r.read_segment_transformed(*r.find(2), transformed));
    REQUIRE(transformed.size() == recs.size());
    for ( size_t row = 0; row < 500; ++row )
	for ( size_t col = 0; col < 8; ++col )
	    CHECK(transformed[col * 500 + row] == recs[row * 8 + col]);
    REQUIRE(r.read_segment(*r.find(1), got));
    CHECK(memcmp(got.data(), idx.data(), idx.size() * 4) == 0);
    REQUIRE(r.read_segment(*r.find(2), got));
    CHECK(got == recs);
    REQUIRE(r.read_segment(*r.find(3), got));
    CHECK(memcmp(got.data(), idx.data(), idx.size() * 4) == 0);
    CHECK(r.raw_ptr(*r.find(3)) == nullptr);   // transformed -> no bind-in-place
}

TEST_CASE("transforms that do not fit the payload are refused")
{
    using madc::dis::snap_xform_flags;
    using madc::dis::SNAP_XFORM_DELTA32;
    using madc::dis::SNAP_XFORM_BYTEPLANE;

    snapshot_writer w;
    // delta32 needs a multiple of 4
    CHECK(!w.add_segment(1, 0, "abcde", 5, PchCompression::None,
			 0, snap_xform_flags(SNAP_XFORM_DELTA32)));
    // byteplane needs a nonzero stride dividing the payload
    CHECK(!w.add_segment(2, 0, "abcdefgh", 8, PchCompression::None,
			 0, snap_xform_flags(SNAP_XFORM_BYTEPLANE, 0)));
    CHECK(!w.add_segment(3, 0, "abcdefgh", 8, PchCompression::None,
			 0, snap_xform_flags(SNAP_XFORM_BYTEPLANE, 3)));
    // unknown transform id / reserved high bits
    CHECK(!w.add_segment(4, 0, "abcd", 4, PchCompression::None, 0, 0x7u));
    CHECK(!w.add_segment(5, 0, "abcd", 4, PchCompression::None, 0, 0x01000001u));
}

TEST_CASE("a corrupt directory record is never handed out")
{
    snapshot_writer w;
    REQUIRE(w.add_segment(1, 0, "abcd", 4, PchCompression::None));
    REQUIRE(w.add_segment(2, 0, "efgh", 4, PchCompression::None));
    std::vector<uint8_t> blob;
    REQUIRE(w.build(blob));

    madc::dis::snapshot_footer ftr;
    memcpy(&ftr, blob.data() + blob.size() - sizeof(ftr), sizeof(ftr));
    snapshot_segment *dir = (snapshot_segment *)(blob.data() + ftr.dir_offset);
    snapshot_reader r;
    dir[0].flags = 0x7u;   // invalid transform id
    REQUIRE(r.open(blob.data(), blob.size()));
    CHECK(r.find(1) == nullptr);
    CHECK(r.segment_at(0) == nullptr);
    CHECK(r.find(2) != nullptr);   // its neighbour is intact
    dir[0].flags = 0;
    const uint64_t comp = dir[0].comp_size;
    dir[0].comp_size = ~(uint64_t)0;   // payload past the table (and wrapping)
    REQUIRE(r.open(blob.data(), blob.size()));
    CHECK(r.find(1) == nullptr);
    dir[0].comp_size = comp;
    REQUIRE(r.open(blob.data(), blob.size()));
    REQUIRE(r.find(1) != nullptr);
    std::vector<uint8_t> got;
    REQUIRE(r.read_segment(*r.find(1), got));
    CHECK(memcmp(got.data(), "abcd", 4) == 0);
}

TEST_CASE("a directory misaligned in the image is copied out and read the same")
{
    snapshot_writer w;
    REQUIRE(w.add_segment(3, 0, "abcd", 4, PchCompression::None));
    REQUIRE(w.add_segment(9, 0, "efghij", 6, PchCompression::None));
    std::vector<uint8_t> blob;
    REQUIRE(w.build(blob));
    std::vector<uint8_t> shifted(blob.size() + 1);
    memcpy(shifted.data() + 1, blob.data(), blob.size());
    snapshot_reader r;
    REQUIRE(r.open(shifted.data() + 1, blob.size()));
    CHECK(r.segment_count() == 2u);
    REQUIRE(r.find(9) != nullptr);
    std::vector<uint8_t> got;
    REQUIRE(r.read_segment(*r.find(9), got));
    CHECK(std::string(got.begin(), got.end()) == "efghij");
    CHECK(r.find(4) == nullptr);
}

// A BLOCKED segment over `raw` with 1 KiB blocks (zlib), built into `blob`.
static const snapshot_segment *blocked_container(const std::vector<uint8_t> &raw,
						 std::vector<uint8_t> &blob,
						 snapshot_reader &r)
{
    snapshot_writer w;
    if ( !w.add_segment(1, 0, raw.data(), raw.size(), PchCompression::Zlib, 0,
			madc::dis::snap_xform_flags(madc::dis::SNAP_XFORM_BLOCKED, 10))
	 || !w.build(blob) || !r.open(blob.data(), blob.size()) )
	return nullptr;
    return r.find(1);
}

static std::vector<uint8_t> blocked_sample()
{
    // Ten compressible 1 KiB blocks and a 100-byte tail under the raw floor.
    std::vector<uint8_t> raw(10 * 1024 + 100);
    for ( size_t i = 0; i < raw.size(); ++i )
	raw[i] = (uint8_t)((i / 7) & 0xff);
    return raw;
}

TEST_CASE("BLOCKED segments decode whole, or block by block on first touch")
{
    std::vector<uint8_t> raw = blocked_sample(), blob;
    snapshot_reader r;
    const snapshot_segment *s = blocked_container(raw, blob, r);
    REQUIRE(s != nullptr);
    CHECK(r.raw_ptr(*s) == nullptr);
    std::vector<uint8_t> got;
    REQUIRE(r.read_segment(*s, got));
    CHECK(got == raw);

    madc::dis::snapshot_lazy_segment *lz = r.open_lazy(*s);
    REQUIRE(lz != nullptr);
    CHECK(lz->size() == raw.size());
    CHECK(lz->block_count() == 11u);
    CHECK(lz->blocks_decoded() == 0u);
    // A range straddling blocks 3 and 4 decodes exactly those two.
    REQUIRE(lz->ensure(3 * 1024 + 1000, 100));
    CHECK(lz->blocks_decoded() == 2u);
    CHECK(memcmp(lz->data() + 3 * 1024 + 1000, raw.data() + 3 * 1024 + 1000, 100) == 0);
    REQUIRE(lz->ensure(3 * 1024, 10));		// already decoded
    CHECK(lz->blocks_decoded() == 2u);
    REQUIRE(lz->ensure(raw.size() - 1, 1));	// the raw-stored tail
    CHECK(lz->blocks_decoded() == 3u);
    CHECK(lz->data()[raw.size() - 1] == raw.back());
    CHECK(!lz->ensure(raw.size(), 1));		// past the end
    CHECK(lz->ensure(raw.size(), 0));
    REQUIRE(lz->ensure(0, raw.size()));
    CHECK(lz->blocks_decoded() == 11u);
    CHECK(memcmp(lz->data(), raw.data(), raw.size()) == 0);
    delete lz;

    // Only a BLOCKED segment has a lazy reader.
    snapshot_writer w;
    REQUIRE(w.add_segment(1, 0, raw.data(), raw.size(), PchCompression::Zlib));
    std::vector<uint8_t> plain;
    REQUIRE(w.build(plain));
    snapshot_reader rp;
    REQUIRE(rp.open(plain.data(), plain.size()));
    CHECK(rp.open_lazy(*rp.find(1)) == nullptr);
}

TEST_CASE("BLOCKED blocks the writer is told to keep raw are stored raw and read the same")
{
    std::vector<uint8_t> raw = blocked_sample(), blob;
    std::vector<bool> keep(11, false);
    keep[0] = keep[4] = true;
    keep.push_back(true);			// past the last block: ignored
    snapshot_writer w;
    REQUIRE(w.add_segment(1, 0, raw.data(), raw.size(), PchCompression::Zlib, 0,
			  madc::dis::snap_xform_flags(madc::dis::SNAP_XFORM_BLOCKED, 10),
			  &keep));
    REQUIRE(w.build(blob));
    snapshot_reader r;
    REQUIRE(r.open(blob.data(), blob.size()));
    const snapshot_segment *s = r.find(1);
    REQUIRE(s != nullptr);
    const uint8_t *payload = blob.data() + s->offset;
    for ( size_t b = 0; b < 10; ++b )
    {
	madc::dis::snapshot_block e;
	memcpy(&e, payload + sizeof(madc::dis::snapshot_block_header) + b * sizeof e, sizeof e);
	CHECK((e.stored == 1024u) == (b == 0 || b == 4));
    }
    std::vector<uint8_t> got;
    REQUIRE(r.read_segment(*s, got));
    CHECK(got == raw);
    madc::dis::snapshot_lazy_segment *lz = r.open_lazy(*s);
    REQUIRE(lz != nullptr);
    REQUIRE(lz->ensure(4 * 1024 + 10, 2000));	// a raw block, then a framed one
    CHECK(memcmp(lz->data() + 4 * 1024 + 10, raw.data() + 4 * 1024 + 10, 2000) == 0);
    delete lz;
}

TEST_CASE("a BLOCKED segment with a corrupt block table is refused")
{
    std::vector<uint8_t> raw = blocked_sample(), blob;
    snapshot_reader r;
    const snapshot_segment *s = blocked_container(raw, blob, r);
    REQUIRE(s != nullptr);
    size_t at = (size_t)s->offset;
    std::vector<uint8_t> got;

    blob[at + 4] ^= 1;				// block_count
    CHECK(r.open_lazy(*s) == nullptr);
    CHECK(!r.read_segment(*s, got));
    blob[at + 4] ^= 1;

    // Block 0's offset pointed past the stored payload.
    size_t e0 = at + sizeof(madc::dis::snapshot_block_header);
    uint32_t saved;
    memcpy(&saved, &blob[e0], 4);
    uint32_t bad = (uint32_t)s->comp_size;
    memcpy(&blob[e0], &bad, 4);
    CHECK(r.open_lazy(*s) == nullptr);
    CHECK(!r.read_segment(*s, got));
    memcpy(&blob[e0], &saved, 4);

    madc::dis::snapshot_lazy_segment *lz = r.open_lazy(*s);
    CHECK(lz != nullptr);
    delete lz;
}

TEST_CASE("BLOCKED inner transforms recode each framed block and read back raw")
{
    using madc::dis::snap_blocked_flags;
    using madc::dis::SNAP_XFORM_NONE;
    using madc::dis::SNAP_XFORM_DELTA32;
    using madc::dis::SNAP_XFORM_BYTEPLANE;

    // u32 child-entry-like words: ascending runs plus a connector-style entry.
    std::vector<uint32_t> idx;
    for ( uint32_t i = 0; i < 2600; ++i )
	idx.push_back(i % 900 == 5 ? 0x80000000u | i : i * 3);
    // 24-byte records: 1 KiB blocks hold 42 whole records and a 16-byte tail.
    std::vector<uint8_t> recs;
    for ( uint32_t i = 0; i < 450; ++i )
	for ( uint32_t c = 0; c < 24; ++c )
	    recs.push_back((uint8_t)(c < 4 ? i >> (8 * c) : c * 7));
    std::vector<bool> keep(11, false);
    keep[2] = true;				// stored raw: no inner recoding

    const uint32_t fd = snap_blocked_flags(10, SNAP_XFORM_DELTA32);
    const uint32_t fb = snap_blocked_flags(10, SNAP_XFORM_BYTEPLANE, 24);
    CHECK(madc::dis::snap_blocked_shift(fb) == 10u);
    CHECK(madc::dis::snap_blocked_stride(fb) == 24u);
    CHECK(madc::dis::snap_blocked_inner(fb) == (uint32_t)SNAP_XFORM_BYTEPLANE);
    snapshot_writer w;
    REQUIRE(w.add_segment(1, 0, idx.data(), idx.size() * 4, PchCompression::Zlib, 0, fd));
    REQUIRE(w.add_segment(2, 0, recs.data(), recs.size(), PchCompression::Zlib, 0, fb, &keep));
    std::vector<uint8_t> blob;
    REQUIRE(w.build(blob));
    snapshot_reader r;
    REQUIRE(r.open(blob.data(), blob.size()));

    std::vector<uint8_t> got;
    REQUIRE(r.read_segment(*r.find(1), got));
    REQUIRE(got.size() == idx.size() * 4);
    CHECK(memcmp(got.data(), idx.data(), got.size()) == 0);
    REQUIRE(r.read_segment(*r.find(2), got));
    CHECK(got == recs);

    madc::dis::snapshot_lazy_segment *lz = r.open_lazy(*r.find(1));
    REQUIRE(lz != nullptr);
    REQUIRE(lz->ensure(5 * 1024 - 8, 16));	// straddles blocks 4 and 5
    CHECK(lz->blocks_decoded() == 2u);
    CHECK(memcmp(lz->data() + 5 * 1024 - 8, (const uint8_t *)idx.data() + 5 * 1024 - 8, 16) == 0);
    delete lz;
    lz = r.open_lazy(*r.find(2));
    REQUIRE(lz != nullptr);
    REQUIRE(lz->ensure(3 * 1024 - 20, 40));	// a framed block's tail, then a framed block
    CHECK(memcmp(lz->data() + 3 * 1024 - 20, recs.data() + 3 * 1024 - 20, 40) == 0);
    REQUIRE(lz->ensure(2 * 1024, 1024));		// the raw-kept block
    CHECK(memcmp(lz->data() + 2 * 1024, recs.data() + 2 * 1024, 1024) == 0);
    REQUIRE(lz->ensure(0, recs.size()));
    CHECK(memcmp(lz->data(), recs.data(), recs.size()) == 0);
    delete lz;

    // Flags that do not fit: block size out of range, a stride without
    // byte-planes, delta over a partial word, a zero stride, an unknown inner.
    snapshot_writer bad;
    CHECK(!bad.add_segment(3, 0, idx.data(), 400, PchCompression::Zlib, 0,
			   snap_blocked_flags(9)));
    CHECK(!bad.add_segment(4, 0, idx.data(), 400, PchCompression::Zlib, 0,
			   snap_blocked_flags(10, SNAP_XFORM_NONE, 8)));
    CHECK(!bad.add_segment(5, 0, idx.data(), 401, PchCompression::Zlib, 0,
			   snap_blocked_flags(10, SNAP_XFORM_DELTA32)));
    CHECK(!bad.add_segment(6, 0, idx.data(), 400, PchCompression::Zlib, 0,
			   snap_blocked_flags(10, SNAP_XFORM_BYTEPLANE, 0)));
    CHECK(!bad.add_segment(7, 0, idx.data(), 400, PchCompression::Zlib, 0,
			   snap_blocked_flags(10, (madc::dis::snapshot_transform)7)));
}

TEST_CASE("a frozen intern table reads through lazy blocks, decoding only what it reads")
{
    intern_table p;
    std::vector<uint32_t> ids;
    populate(p, ids, 3000);

    snapshot_writer w;
    std::vector<char> bytes;
    std::vector<uint32_t> offs;
    p.frozen_blocks(bytes, offs);
    uint32_t xf = madc::dis::snap_xform_flags(madc::dis::SNAP_XFORM_BLOCKED, 10);
    REQUIRE(w.add_segment(1, madc::dis::SNAP_KIND_INTERN_BYTES,
			  bytes.data(), bytes.size(), PchCompression::Zlib, 0, xf));
    std::vector<uint8_t> blob;
    REQUIRE(w.build(blob));
    snapshot_reader r;
    REQUIRE(r.open(blob.data(), blob.size()));
    madc::dis::snapshot_lazy_segment *lb = r.open_lazy(*r.find(1));
    REQUIRE(lb != nullptr);

    frozen_intern_table fz;
    fz.bind((const char *)lb->data(), lb->size(), offs.data(), offs.size(), lb);
    CHECK(fz.valid());
    CHECK(lb->blocks_decoded() == 0u);	// validating decodes nothing
    CHECK(fz.count() == p.count());

    // One spelling decodes the block(s) holding it — not the table.
    CHECK(fz.str(ids[2500]) == "spelling_2500");
    CHECK(lb->blocks_decoded() <= 3u);
    CHECK(lb->blocks_decoded() < lb->block_count() / 4);

    char buf[32];
    for ( int i = 0; i < 3000; ++i )
    {
	snprintf(buf, sizeof buf, "spelling_%d", i);
	CHECK(fz.str(ids[(size_t)i]) == buf);
    }
    CHECK(lb->blocks_decoded() == lb->block_count());
    // An id past the table reads as the empty spelling, never out of bounds.
    CHECK(std::string(fz.c_str((uint32_t)p.count() + 7)) == "");
    CHECK(fz.length((uint32_t)p.count() + 7) == 0u);
    delete lb;
}
