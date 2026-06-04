// Unit tests for the first fixed-record libmadc storage backend: flr://

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "libmadc/dataset.h"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

enum class StorageStatus : int32_t
{
    unknown = 0,
    active = 1,
    disabled = 2
};

struct StorageProbe
{
    int64_t id;
    int32_t score;
    uint16_t flags;
    bool enabled;
    StorageStatus status;
    char tag;
    char short_name[16];
    std::string title;
    double ratio;
};

namespace {

std::string short_name_string(const StorageProbe &p)
{
    const char *start = p.short_name;
    std::size_t len = 0;
    while ( len < sizeof(p.short_name) && start[len] != '\0' )
	++len;
    return std::string(start, len);
}

void assign_short_name(StorageProbe &p, const std::string &name)
{
    std::size_t i = 0;
    for ( ; i + 1 < sizeof(p.short_name) && i < name.size(); ++i )
	p.short_name[i] = name[i];
    p.short_name[i] = '\0';
    for ( ++i; i < sizeof(p.short_name); ++i )
	p.short_name[i] = '\0';
}

StorageProbe make_probe(int64_t id,
			int32_t score,
			uint16_t flags,
			bool enabled,
			StorageStatus status,
			char tag,
			const std::string &short_name,
			const std::string &title,
			double ratio)
{
    StorageProbe p;
    p.id = id;
    p.score = score;
    p.flags = flags;
    p.enabled = enabled;
    p.status = status;
    p.tag = tag;
    assign_short_name(p, short_name);
    p.title = title;
    p.ratio = ratio;
    return p;
}

std::size_t file_size(const std::string &path)
{
    std::ifstream is(path.c_str(), std::ios::binary);
    is.seekg(0, std::ios::end);
    return static_cast<std::size_t>(is.tellg());
}

} // namespace

namespace madc {

template <>
struct MapperRegistration<StorageProbe>
{
    static std::shared_ptr<DataMapper<StorageProbe> > make(const MappingSpec<StorageProbe> &spec)
    {
	(void)spec;
	MapperBuilder<StorageProbe> builder("StorageProbe");
	builder.field("id", &StorageProbe::id, true, 0);
	builder.field("score", &StorageProbe::score, false, 8);
	builder.field("flags", &StorageProbe::flags, false, 12);
	builder.field("enabled", &StorageProbe::enabled, false, 14);
	builder.field("tag", &StorageProbe::tag, false, 15);
	builder.field("status", &StorageProbe::status, false, 16);
	builder.field("short_name", &StorageProbe::short_name, false, 20);
	builder.field("title", &StorageProbe::title, 20, false, 36);
	builder.field("ratio", &StorageProbe::ratio, false, 56);
	return builder.build();
    }
};

} // namespace madc

namespace {

std::size_t count_cursor_rows(std::unique_ptr<madc::Cursor<StorageProbe> > cur)
{
    std::size_t count = 0;
    StorageProbe row;
    while ( cur.get() && cur->next(row) )
	++count;
    if ( cur.get() )
	cur->close();
    return count;
}

} // namespace

TEST_SUITE("libmadc flr backend") {

    TEST_CASE("flr dataset round-trips fixed records and truncates only when configured") {
	const std::string path =
	    "/tmp/madc_flr_probe_" + std::to_string(static_cast<long long>(getpid())) + ".bin";
	std::remove(path.c_str());

	madc::MappingSpec<StorageProbe> spec;
	spec.key("id")
	    .fixed_record_size(64)
	    .text_field_limit("title", 20,
				madc::MappingSpec<StorageProbe>::overflow_policy::truncate);

	madc::DataSet<StorageProbe> ds("flr://" + path);
	ds.mapping(spec).name("users");

	StorageProbe p = make_probe(7, 123, 4, true, StorageStatus::active, 'Z',
				    "ZEUS", "This title is definitely too long", 4.5);

	madc::error err;
	REQUIRE(ds.insert(p, &err));
	CHECK(file_size(path) == 64);

	StorageProbe fetched;
	REQUIRE(ds.get(madc::value(int64_t(7)), fetched, &err));
	CHECK(fetched.id == 7);
	CHECK(fetched.score == 123);
	CHECK(fetched.flags == 4);
	CHECK(fetched.enabled == true);
	CHECK(fetched.tag == 'Z');
	CHECK(short_name_string(fetched) == "ZEUS");
	CHECK(fetched.title == "This title is defin");
	CHECK(fetched.ratio == doctest::Approx(4.5));

	std::unique_ptr<madc::Cursor<StorageProbe> > rows = ds.scan(&err);
	REQUIRE(static_cast<bool>(rows));
	CHECK(count_cursor_rows(std::move(rows)) == 1);

	std::remove(path.c_str());
    }

    TEST_CASE("flr strict mode rejects oversized string fields") {
	const std::string path =
	    "/tmp/madc_flr_strict_" + std::to_string(static_cast<long long>(getpid())) + ".bin";
	std::remove(path.c_str());

	madc::MappingSpec<StorageProbe> spec;
	spec.key("id")
	    .fixed_record_size(64);

	madc::DataSet<StorageProbe> ds("flr://" + path);
	ds.mapping(spec);

	StorageProbe p = make_probe(8, 1, 2, false, StorageStatus::disabled, 'Q',
				    "QUERY", "This title is definitely too long", 2.0);

	madc::error err;
	CHECK_FALSE(ds.insert(p, &err));
	CHECK(err.message.find("exceeds fixed size") != std::string::npos);
	std::remove(path.c_str());
    }

    TEST_CASE("flr tombstone sidecar hides deleted rows and supports undelete before reap") {
	const std::string path =
	    "/tmp/madc_flr_tomb_" + std::to_string(static_cast<long long>(getpid())) + ".bin";
	const std::string tombstones =
	    "/tmp/madc_flr_tomb_" + std::to_string(static_cast<long long>(getpid())) + ".bits";
	std::remove(path.c_str());
	std::remove(tombstones.c_str());

	madc::MappingSpec<StorageProbe> spec;
	spec.key("id")
	    .fixed_record_size(64)
	    .tombstone_file(tombstones);

	StorageProbe alice = make_probe(1, 10, 7, true, StorageStatus::active, 'A',
					"ALPHA", "Alice Example", 1.25);
	StorageProbe bob = make_probe(2, 20, 9, false, StorageStatus::disabled, 'B',
				      "BRAVO", "Bob Example", 2.5);

	madc::error err;
	{
	    madc::DataSet<StorageProbe> ds("flr://" + path);
	    ds.mapping(spec).name("users");

	    REQUIRE(ds.insert(alice, &err));
	    REQUIRE(ds.insert(bob, &err));
	    CHECK(file_size(path) == 128);
	    CHECK(file_size(tombstones) == 1);
	    CHECK(ds.size(&err) == 2);

	    REQUIRE(ds.erase(madc::value(int64_t(2)), &err));
	    CHECK(file_size(path) == 128);
	    CHECK(file_size(tombstones) == 1);
	    CHECK(ds.size(&err) == 1);

	    StorageProbe fetched;
	    CHECK_FALSE(ds.get(madc::value(int64_t(2)), fetched, &err));
	    REQUIRE(ds.get(madc::value(int64_t(1)), fetched, &err));
	    CHECK(fetched.title == "Alice Example");
	}

	{
	    madc::DataSet<StorageProbe> ds("flr://" + path);
	    ds.mapping(spec).name("users");

	    CHECK(ds.size(&err) == 1);
	    StorageProbe fetched;
	    CHECK_FALSE(ds.get(madc::value(int64_t(2)), fetched, &err));

	    REQUIRE(ds.restore(madc::value(int64_t(2)), &err));
	    CHECK(ds.size(&err) == 2);
	    REQUIRE(ds.get(madc::value(int64_t(2)), fetched, &err));
	    CHECK(fetched.title == "Bob Example");
	}

	std::remove(path.c_str());
	std::remove(tombstones.c_str());
    }

    TEST_CASE("flr compact reaps tombstoned rows into dead archive and shrinks live file") {
	const std::string path =
	    "/tmp/madc_flr_compact_" + std::to_string(static_cast<long long>(getpid())) + ".bin";
	const std::string tombstones =
	    "/tmp/madc_flr_compact_" + std::to_string(static_cast<long long>(getpid())) + ".bits";
	const std::string dead =
	    "/tmp/madc_flr_compact_" + std::to_string(static_cast<long long>(getpid())) + ".dead";
	std::remove(path.c_str());
	std::remove(tombstones.c_str());
	std::remove(dead.c_str());

	madc::MappingSpec<StorageProbe> live_spec;
	live_spec.key("id")
		 .fixed_record_size(64)
		 .tombstone_file(tombstones)
		 .dead_record_file(dead);

	madc::MappingSpec<StorageProbe> archive_spec;
	archive_spec.key("id")
		    .fixed_record_size(64);

	StorageProbe alice = make_probe(1, 10, 7, true, StorageStatus::active, 'A',
					"ALPHA", "Alice Example", 1.25);
	StorageProbe bob = make_probe(2, 20, 9, false, StorageStatus::disabled, 'B',
				      "BRAVO", "Bob Example", 2.5);
	StorageProbe cara = make_probe(3, 30, 11, true, StorageStatus::active, 'C',
				       "CHARLIE", "Cara Example", 3.75);

	madc::error err;
	{
	    madc::DataSet<StorageProbe> ds("flr://" + path);
	    ds.mapping(live_spec).name("users");

	    REQUIRE(ds.insert(alice, &err));
	    REQUIRE(ds.insert(bob, &err));
	    REQUIRE(ds.insert(cara, &err));
	    REQUIRE(ds.erase(madc::value(int64_t(2)), &err));
	    CHECK(ds.size(&err) == 2);

	    REQUIRE(ds.compact(&err));
	    CHECK(ds.size(&err) == 2);
	    CHECK(file_size(path) == 128);
	    CHECK(file_size(dead) == 64);
	    CHECK(file_size(tombstones) == 1);

	    StorageProbe fetched;
	    CHECK_FALSE(ds.get(madc::value(int64_t(2)), fetched, &err));
	    REQUIRE(ds.get(madc::value(int64_t(1)), fetched, &err));
	    CHECK(fetched.title == "Alice Example");
	    REQUIRE(ds.get(madc::value(int64_t(3)), fetched, &err));
	    CHECK(fetched.title == "Cara Example");
	}

	{
	    madc::DataSet<StorageProbe> ds("flr://" + path);
	    ds.mapping(live_spec).name("users");
	    madc::DataSet<StorageProbe> archive("flr://" + dead);
	    archive.mapping(archive_spec).name("dead_users");

	    madc::error archive_err;
	    CHECK(ds.size(&err) == 2);
	    CHECK(archive.size(&archive_err) == 1);

	    StorageProbe fetched;
	    REQUIRE(archive.get(madc::value(int64_t(2)), fetched, &archive_err));
	    CHECK(fetched.title == "Bob Example");
	    CHECK(short_name_string(fetched) == "BRAVO");
	}

	std::remove(path.c_str());
	std::remove(tombstones.c_str());
	std::remove(dead.c_str());
    }

    TEST_CASE("flr restore after reap reinserts archived row in ordered key position") {
	const std::string path =
	    "/tmp/madc_flr_restore_" + std::to_string(static_cast<long long>(getpid())) + ".bin";
	const std::string tombstones =
	    "/tmp/madc_flr_restore_" + std::to_string(static_cast<long long>(getpid())) + ".bits";
	const std::string dead =
	    "/tmp/madc_flr_restore_" + std::to_string(static_cast<long long>(getpid())) + ".dead";
	std::remove(path.c_str());
	std::remove(tombstones.c_str());
	std::remove(dead.c_str());

	madc::MappingSpec<StorageProbe> live_spec;
	live_spec.key("id")
		 .fixed_record_size(64)
		 .ordered_by("id", madc::SchemaInfo::key_compare::numeric_signed)
		 .tombstone_file(tombstones)
		 .dead_record_file(dead);

	madc::MappingSpec<StorageProbe> archive_spec;
	archive_spec.key("id")
		    .fixed_record_size(64)
		    .ordered_by("id", madc::SchemaInfo::key_compare::numeric_signed);

	StorageProbe one = make_probe(1, 10, 7, true, StorageStatus::active, 'A',
				      "ALPHA", "Alice Example", 1.25);
	StorageProbe two = make_probe(2, 20, 9, false, StorageStatus::disabled, 'B',
				      "BRAVO", "Bob Example", 2.5);
	StorageProbe three = make_probe(3, 30, 11, true, StorageStatus::active, 'C',
					"CHARLIE", "Cara Example", 3.75);

	madc::error err;
	{
	    madc::DataSet<StorageProbe> ds("flr://" + path);
	    ds.mapping(live_spec).name("users");

	    REQUIRE(ds.insert(one, &err));
	    REQUIRE(ds.insert(two, &err));
	    REQUIRE(ds.insert(three, &err));
	    REQUIRE(ds.erase(madc::value(int64_t(2)), &err));
	    REQUIRE(ds.compact(&err));
	    CHECK(ds.size(&err) == 2);

	    REQUIRE(ds.restore(madc::value(int64_t(2)), &err));
	    CHECK(ds.size(&err) == 3);
	}

	{
	    madc::DataSet<StorageProbe> ds("flr://" + path);
	    ds.mapping(live_spec).name("users");
	    madc::DataSet<StorageProbe> archive("flr://" + dead);
	    archive.mapping(archive_spec).name("dead_users");

	    CHECK(ds.size(&err) == 3);
	    CHECK(archive.size(&err) == 0);

	    StorageProbe fetched;
	    REQUIRE(ds.get(madc::value(int64_t(2)), fetched, &err));
	    CHECK(fetched.title == "Bob Example");
	    CHECK(short_name_string(fetched) == "BRAVO");

	    std::vector<int64_t> ids;
	    for (madc::DataSet<StorageProbe>::iterator it = ds.begin(&err);
		 it != ds.end(&err); ++it)
		ids.push_back(it->id);
	    REQUIRE(ids.size() == 3);
	    CHECK(ids[0] == 1);
	    CHECK(ids[1] == 2);
	    CHECK(ids[2] == 3);
	}

	std::remove(path.c_str());
	std::remove(tombstones.c_str());
	std::remove(dead.c_str());
    }
}
