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

TEST_SUITE("libmadc qdbm backend") {

#ifdef HAVE_QDBM
    TEST_CASE("qdbm dataset stores keyed records in Villa order") {
	const std::string path =
	    "/tmp/madc_qdbm_probe_" + std::to_string(static_cast<long long>(getpid())) + ".villa";
	std::remove(path.c_str());

	madc::MappingSpec<StorageProbe> spec;
	spec.key("id");

	madc::DataSet<StorageProbe> ds("qdbm://" + path);
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
	REQUIRE(ds.get(madc::value(int64_t(2)), fetched, &err));
	CHECK(fetched.title == "Bob Example");
	CHECK(short_name_string(fetched) == "BRAVO");

	std::vector<int64_t> ids;
	for (const auto &row : ds)
	    ids.push_back(row.id);
	REQUIRE(ids.size() == 3);
	CHECK(ids[0] == 1);
	CHECK(ids[1] == 2);
	CHECK(ids[2] == 3);

	bob.score = 99;
	bob.enabled = true;
	bob.title = "Bob Updated";
	REQUIRE(ds.update(bob, &err));
	REQUIRE(ds.get(madc::value(int64_t(2)), fetched, &err));
	CHECK(fetched.score == 99);
	CHECK(fetched.enabled == true);
	CHECK(fetched.title == "Bob Updated");

	REQUIRE(ds.erase(madc::value(int64_t(2)), &err));
	CHECK_FALSE(ds.contains(madc::value(int64_t(2)), &err));
	CHECK(ds.size(&err) == 2);

	std::unique_ptr<madc::Cursor<StorageProbe> > pushed =
	    ds.query(madc::query().from("users").where_eq("id", madc::value(int64_t(1))).limit(1).build(), &err);
	REQUIRE(static_cast<bool>(pushed));
	StorageProbe row;
	REQUIRE(pushed->next(row));
	CHECK(row.id == 1);
	CHECK(row.title == "Alice Example");
	CHECK_FALSE(pushed->next(row));
	pushed->close();

	std::unique_ptr<madc::Cursor<StorageProbe> > ranged =
	    ds.query(madc::query().from("users").where_gte("id", madc::value(int64_t(2))).limit(2).build(), &err);
	REQUIRE(static_cast<bool>(ranged));
	REQUIRE(ranged->next(row));
	CHECK(row.id == 3);
	CHECK(row.title == "Cara Example");
	CHECK_FALSE(ranged->next(row));
	ranged->close();

	std::unique_ptr<madc::Cursor<StorageProbe> > strict_lower =
	    ds.query(madc::query().from("users").where_gt("id", madc::value(int64_t(1))).limit(2).build(), &err);
	REQUIRE(static_cast<bool>(strict_lower));
	REQUIRE(strict_lower->next(row));
	CHECK(row.id == 3);
	CHECK(row.title == "Cara Example");
	CHECK_FALSE(strict_lower->next(row));
	strict_lower->close();

	std::unique_ptr<madc::Cursor<StorageProbe> > strict_upper =
	    ds.query(madc::query().from("users").where_lt("id", madc::value(int64_t(3))).limit(2).build(), &err);
	REQUIRE(static_cast<bool>(strict_upper));
	REQUIRE(strict_upper->next(row));
	CHECK(row.id == 1);
	CHECK(row.title == "Alice Example");
	CHECK_FALSE(strict_upper->next(row));
	strict_upper->close();

	    std::unique_ptr<madc::Cursor<StorageProbe> > bounded =
	        ds.query(madc::query().from("users").where_gte("id", madc::value(int64_t(1))).where_lte("id", madc::value(int64_t(2))).limit(2).build(), &err);
	    REQUIRE(static_cast<bool>(bounded));
	    REQUIRE(bounded->next(row));
	    CHECK(row.id == 1);
	    CHECK(row.title == "Alice Example");
	    CHECK_FALSE(bounded->next(row));
	    bounded->close();

	    std::unique_ptr<madc::Cursor<StorageProbe> > not_equal =
		ds.query(madc::query().from("users").where_ne("title", madc::value("Alice Example")).limit(2).build(), &err);
	    REQUIRE(static_cast<bool>(not_equal));
	    REQUIRE(not_equal->next(row));
	    CHECK(row.id == 3);
	    CHECK(row.title == "Cara Example");
	    CHECK_FALSE(not_equal->next(row));
	    not_equal->close();

	    std::vector<madc::value> match_ids;
	    match_ids.push_back(madc::value(int64_t(1)));
	    match_ids.push_back(madc::value(int64_t(3)));
	    std::unique_ptr<madc::Cursor<StorageProbe> > in_rows =
		ds.query(madc::query().from("users").where_in("id", match_ids).limit(2).build(), &err);
	    REQUIRE(static_cast<bool>(in_rows));
	    REQUIRE(in_rows->next(row));
	    CHECK(row.id == 1);
	    REQUIRE(in_rows->next(row));
	    CHECK(row.id == 3);
	    CHECK_FALSE(in_rows->next(row));
	    in_rows->close();

	    std::vector<madc::value> excluded_ids;
	    excluded_ids.push_back(madc::value(int64_t(1)));
	    std::unique_ptr<madc::Cursor<StorageProbe> > not_in_rows =
		ds.query(madc::query().from("users").where_not_in("id", excluded_ids).limit(2).build(), &err);
	    REQUIRE(static_cast<bool>(not_in_rows));
	    REQUIRE(not_in_rows->next(row));
	    CHECK(row.id == 3);
	    CHECK(row.title == "Cara Example");
	    CHECK_FALSE(not_in_rows->next(row));
	    not_in_rows->close();

	    std::unique_ptr<madc::Cursor<StorageProbe> > like_rows =
		ds.query(madc::query().from("users").where_like("title", madc::value("C%")).limit(1).build(), &err);
	    REQUIRE(static_cast<bool>(like_rows));
	    REQUIRE(like_rows->next(row));
	    CHECK(row.id == 3);
	    CHECK(row.title == "Cara Example");
	    CHECK_FALSE(like_rows->next(row));
	    like_rows->close();

	    std::unique_ptr<madc::Cursor<madc::value> > projected =
		ds.query_raw(madc::query().from("users").where_eq("id", madc::value(int64_t(1))).select(std::vector<std::string>{"id", "title"}).limit(1).build(), &err);
	    REQUIRE(static_cast<bool>(projected));
	    madc::value projected_row;
	    REQUIRE(projected->next(projected_row));
	    REQUIRE(projected_row.is_object());
	    CHECK(projected_row.as_object().count("id") == 1);
	    CHECK(projected_row.as_object().count("title") == 1);
	    CHECK(projected_row.as_object().count("score") == 0);
	    CHECK(projected_row.as_object().at("id") == madc::value(int64_t(1)));
	    CHECK(projected_row.as_object().at("title") == madc::value("Alice Example"));
	    CHECK_FALSE(projected->next(projected_row));
	    projected->close();

	std::remove(path.c_str());
    }
#else
    TEST_CASE("qdbm backend is not built in this configuration") {
	CHECK(true);
    }
#endif
}
