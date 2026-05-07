#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "libmadc/dataset.h"

#include <cstdio>
#include <cstdint>
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

void assign_short_name(StorageProbe &p, const std::string &name)
{
    std::size_t i = 0;
    for ( ; i + 1 < sizeof(p.short_name) && i < name.size(); ++i )
	p.short_name[i] = name[i];
    p.short_name[i] = '\0';
    for ( ++i; i < sizeof(p.short_name); ++i )
	p.short_name[i] = '\0';
}

std::string short_name_string(const StorageProbe &p)
{
    std::size_t len = 0;
    while ( len < sizeof(p.short_name) && p.short_name[len] != '\0' )
	++len;
    return std::string(p.short_name, len);
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

bool has_id(const std::vector<int64_t> &ids, int64_t id)
{
    for ( std::size_t i = 0; i < ids.size(); ++i )
    {
	if ( ids[i] == id )
	    return true;
    }
    return false;
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

TEST_SUITE("libmadc gdbm backend") {

#ifdef HAVE_GDBM
    TEST_CASE("gdbm dataset stores keyed records without ordered-scan guarantees") {
	const std::string path =
	    "/tmp/madc_gdbm_probe_" + std::to_string(static_cast<long long>(getpid())) + ".gdbm";
	std::remove(path.c_str());

	madc::MappingSpec<StorageProbe> spec;
	spec.key("id");

	madc::DataSet<StorageProbe> ds("gdbm://" + path);
	ds.mapping(spec).name("users");

	StorageProbe bob = make_probe(2, 42, 9, false, StorageStatus::disabled, 'B',
				      "BRAVO", "Bob Example", 9.75);
	StorageProbe cara = make_probe(3, 77, 11, true, StorageStatus::active, 'C',
				       "CHARLIE", "Cara Example", 7.5);
	StorageProbe alice = make_probe(1, 10, 7, true, StorageStatus::active, 'A',
					"ALPHA", "Alice Example", 1.25);

	madc::error err;
	REQUIRE(ds.insert(bob, &err));
	REQUIRE(ds.insert(cara, &err));
	REQUIRE(ds.insert(alice, &err));
	CHECK_FALSE(ds.insert(alice, &err));

	StorageProbe fetched;
	REQUIRE(ds.get(madc::value(int64_t(1)), fetched, &err));
	CHECK(fetched.title == "Alice Example");
	CHECK(short_name_string(fetched) == "ALPHA");

	alice.score = 88;
	alice.enabled = false;
	alice.title = "Alice Updated";
	REQUIRE(ds.update(alice, &err));
	REQUIRE(ds.get(madc::value(int64_t(1)), fetched, &err));
	CHECK(fetched.score == 88);
	CHECK(fetched.enabled == false);
	CHECK(fetched.title == "Alice Updated");

	std::vector<int64_t> ids;
	for (const auto &row : ds)
	    ids.push_back(row.id);
	REQUIRE(ids.size() == 3);
	CHECK(has_id(ids, 1));
	CHECK(has_id(ids, 2));
	CHECK(has_id(ids, 3));

	REQUIRE(ds.erase(madc::value(int64_t(2)), &err));
	CHECK_FALSE(ds.contains(madc::value(int64_t(2)), &err));
	CHECK(ds.size(&err) == 2);

	std::unique_ptr<madc::Cursor<StorageProbe> > pushed =
	    ds.query(madc::query().from("users").where_eq("id", madc::value(int64_t(3))).limit(1).build(), &err);
	REQUIRE(static_cast<bool>(pushed));
	StorageProbe row;
	REQUIRE(pushed->next(row));
	CHECK(row.id == 3);
	CHECK(row.title == "Cara Example");
	CHECK_FALSE(pushed->next(row));
	pushed->close();

	std::remove(path.c_str());
    }
#else
    TEST_CASE("gdbm backend is not built in this configuration") {
	CHECK(true);
    }
#endif
}
