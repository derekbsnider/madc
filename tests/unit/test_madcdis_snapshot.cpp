// Unit tests for the madc::dis pool-snapshot container (madcdis/snapshot.h)
// and the frozen intern-table tier (madcdis/intern_table.h): writer/reader
// round-trip, both placements (standalone file / appended-to-binary), codec
// paths, corruption rejection, and frozen_intern_table bind + find parity
// with the live table it was serialized from.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <cstdio>
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

// Serialize a live table's three blocks into a writer (the B2 consumer shape).
static bool add_intern_segments(snapshot_writer &w, const intern_table &p,
				PchCompression codec)
{
    if ( !w.add_segment(1, madc::dis::SNAP_KIND_INTERN_BYTES,
			p.bytes_data(), p.bytes_size(), codec) )
	return false;
    if ( !w.add_segment(2, madc::dis::SNAP_KIND_INTERN_ENTRIES,
			p.entries_data(), p.entries_size() * sizeof(intern_table::Entry),
			codec) )
	return false;
    return w.add_segment(3, madc::dis::SNAP_KIND_INTERN_BUCKETS,
			 p.buckets_data(), p.buckets_size() * sizeof(uint32_t), codec);
}

// Bind a frozen view over three decompressed block buffers.
static void bind_frozen(frozen_intern_table &fz,
			const std::vector<uint8_t> &bytes,
			const std::vector<uint8_t> &entries,
			const std::vector<uint8_t> &buckets)
{
    fz.bind((const char *)bytes.data(), bytes.size(),
	    (const intern_table::Entry *)entries.data(),
	    entries.size() / sizeof(intern_table::Entry),
	    (const uint32_t *)buckets.data(),
	    buckets.size() / sizeof(uint32_t));
}

TEST_CASE("frozen view over a live table's blocks: ids, spellings, find parity")
{
    intern_table p;
    std::vector<uint32_t> ids;
    populate(p, ids, 3000);   // enough to force growth + rehash

    frozen_intern_table fz;
    fz.bind(p.bytes_data(), p.bytes_size(),
	    p.entries_data(), p.entries_size(),
	    p.buckets_data(), p.buckets_size());
    CHECK(fz.valid());
    CHECK(fz.count() == p.count());

    char buf[32];
    for ( int i = 0; i < 3000; ++i )
    {
	uint32_t id = ids[(size_t)i];
	snprintf(buf, sizeof buf, "spelling_%d", i);
	CHECK(fz.str(id) == buf);
	CHECK(fz.length(id) == p.length(id));
	CHECK(fz.find(buf, (uint32_t)strlen(buf)) == id);
    }
    CHECK(fz.find(std::string("absent_spelling")) == (uint32_t)frozen_intern_table::npos);
    CHECK(fz.find("", 0) == 0u);   // empty -> reserved id 0
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
    CHECK(r.segment_count() == 3u);
    CHECK(r.context_hash() == 0x1122334455667788ull);

    std::vector<uint8_t> bytes, entries, buckets;
    REQUIRE(r.find(1) != nullptr);
    REQUIRE(r.find(2) != nullptr);
    REQUIRE(r.find(3) != nullptr);
    CHECK(r.find(1)->kind == (uint32_t)madc::dis::SNAP_KIND_INTERN_BYTES);
    REQUIRE(r.read_segment(*r.find(1), bytes));
    REQUIRE(r.read_segment(*r.find(2), entries));
    REQUIRE(r.read_segment(*r.find(3), buckets));
    CHECK(r.raw_ptr(*r.find(1)) == nullptr);   // compressed -> no bind-in-place

    frozen_intern_table fz;
    bind_frozen(fz, bytes, entries, buckets);
    CHECK(fz.valid());
    CHECK(fz.count() == p.count());
    for ( size_t i = 0; i < ids.size(); ++i )
	CHECK(fz.str(ids[i]) == p.str(ids[i]));
    CHECK(fz.find(std::string("spelling_123")) == p.intern("spelling_123"));
    CHECK(fz.find(std::string("never_interned")) == (uint32_t)frozen_intern_table::npos);
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
    const snapshot_segment *se = r.find(2);
    const snapshot_segment *sk = r.find(3);
    REQUIRE(sb != nullptr);
    REQUIRE(se != nullptr);
    REQUIRE(sk != nullptr);
    CHECK(sb->comp_size == sb->raw_size);
    CHECK((se->offset & 15u) == 0u);   // 16-aligned payloads (bind-in-place contract)
    CHECK((sk->offset & 15u) == 0u);

    frozen_intern_table fz;
    fz.bind((const char *)r.raw_ptr(*sb), (size_t)sb->raw_size,
	    (const intern_table::Entry *)r.raw_ptr(*se),
	    (size_t)se->raw_size / sizeof(intern_table::Entry),
	    (const uint32_t *)r.raw_ptr(*sk),
	    (size_t)sk->raw_size / sizeof(uint32_t));
    CHECK(fz.valid());
    for ( size_t i = 0; i < ids.size(); ++i )
	CHECK(fz.str(ids[i]) == p.str(ids[i]));
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
    CHECK(r.segment_count() == 3u);
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
    CHECK(r.segment_count() == 3u);

    // The blob base must have landed 16-aligned in the file (append pads),
    // so a None-codec payload binds in place even from the appended image.
    const snapshot_segment *se = r.find(2);
    REQUIRE(se != nullptr);
    const uint8_t *pe = r.raw_ptr(*se);
    REQUIRE(pe != nullptr);
    CHECK((((uintptr_t)pe - (uintptr_t)image.data()) & 15u) == 0u);

    std::vector<uint8_t> bytes, entries, buckets;
    REQUIRE(r.read_segment(*r.find(1), bytes));
    REQUIRE(r.read_segment(*r.find(2), entries));
    REQUIRE(r.read_segment(*r.find(3), buckets));
    frozen_intern_table fz;
    bind_frozen(fz, bytes, entries, buckets);
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

TEST_CASE("a corrupt transform in the directory is rejected at open()")
{
    snapshot_writer w;
    REQUIRE(w.add_segment(1, 0, "abcd", 4, PchCompression::None));
    std::vector<uint8_t> blob;
    REQUIRE(w.build(blob));

    madc::dis::snapshot_footer ftr;
    memcpy(&ftr, blob.data() + blob.size() - sizeof(ftr), sizeof(ftr));
    snapshot_segment *dir = (snapshot_segment *)(blob.data() + ftr.dir_offset);
    dir[0].flags = 0x7u;   // invalid transform id
    snapshot_reader r;
    CHECK(!r.open(blob.data(), blob.size()));
    dir[0].flags = 0;
    CHECK(r.open(blob.data(), blob.size()));
}
