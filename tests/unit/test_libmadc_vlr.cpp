// Unit tests for the first variable-record libmadc storage backend: vlr://

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "libmadc/dataset.h"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

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
	MADC_MAP_KEY_FIELD(builder, StorageProbe, id);
	MADC_MAP_FIELD(builder, StorageProbe, score);
	MADC_MAP_FIELD(builder, StorageProbe, flags);
	MADC_MAP_FIELD(builder, StorageProbe, enabled);
	MADC_MAP_FIELD(builder, StorageProbe, status);
	MADC_MAP_FIELD(builder, StorageProbe, tag);
	MADC_MAP_FIELD(builder, StorageProbe, short_name);
	MADC_MAP_FIELD(builder, StorageProbe, title);
	MADC_MAP_FIELD(builder, StorageProbe, ratio);
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

TEST_SUITE("libmadc vlr backend") {

    TEST_CASE("vlr dataset round-trips variable-sized records without truncating strings") {
	const std::string path =
	    "/tmp/madc_vlr_probe_" + std::to_string(static_cast<long long>(getpid())) + ".bin";
	std::remove(path.c_str());

	madc::MappingSpec<StorageProbe> spec;
	spec.key("id")
	    .variable_record();

	madc::DataSet<StorageProbe> ds("vlr://" + path);
	ds.mapping(spec).name("users");

	StorageProbe alice = make_probe(1, 10, 7, true, StorageStatus::active, 'A',
					"ALPHA", "Alice title with plenty of room", 1.25);
	StorageProbe bob = make_probe(2, 42, 9, false, StorageStatus::disabled, 'B',
				      "BRAVO", "Bob title is also intentionally long", 9.75);

	madc::error err;
	REQUIRE(ds.insert(alice, &err));
	REQUIRE(ds.insert(bob, &err));
	CHECK(file_size(path) > 0);

	StorageProbe fetched;
	REQUIRE(ds.get(madc::value(int64_t(2)), fetched, &err));
	CHECK(fetched.id == 2);
	CHECK(fetched.score == 42);
	CHECK(fetched.enabled == false);
	CHECK(short_name_string(fetched) == "BRAVO");
	CHECK(fetched.title == "Bob title is also intentionally long");
	CHECK(fetched.ratio == doctest::Approx(9.75));

	bob.title = "Bob title updated and still long";
	bob.enabled = true;
	REQUIRE(ds.update(bob, &err));
	REQUIRE(ds.get(madc::value(int64_t(2)), fetched, &err));
	CHECK(fetched.title == "Bob title updated and still long");
	CHECK(fetched.enabled == true);

	std::unique_ptr<madc::Cursor<StorageProbe> > filtered =
	    ds.where_eq("short_name", madc::value("ALPHA"), &err);
	REQUIRE(static_cast<bool>(filtered));
	CHECK(count_cursor_rows(std::move(filtered)) == 1);

	REQUIRE(ds.erase(madc::value(int64_t(1)), &err));
	std::unique_ptr<madc::Cursor<StorageProbe> > rows = ds.scan(&err);
	REQUIRE(static_cast<bool>(rows));
	CHECK(count_cursor_rows(std::move(rows)) == 1);

	std::remove(path.c_str());
    }
}
