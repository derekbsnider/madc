// Unit tests for the first real libmadc storage backend: dsv://

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "libmadc/dataset.h"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
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

std::string read_file(const std::string &path)
{
    std::ifstream is(path.c_str());
    std::ostringstream os;
    os << is.rdbuf();
    return os.str();
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

TEST_SUITE("libmadc dsv backend") {

    TEST_CASE("dsv dataset round-trips mixed records through libmadc C++ API") {
	const std::string path =
	    "/tmp/madc_dsv_probe_" + std::to_string(static_cast<long long>(getpid())) + ".csv";
	std::remove(path.c_str());

	madc::MappingSpec<StorageProbe> spec;
	spec.key("id")
	    .field_name("short_name", "shortName");

	madc::DataSet<StorageProbe> ds("dsv://" + path);
	ds.mapping(spec).name("users");

	StorageProbe alice = make_probe(1, 10, 7, true, StorageStatus::active, 'A',
					"ALPHA", "Alice Example", 1.25);
	StorageProbe bob = make_probe(2, 42, 9, false, StorageStatus::disabled, 'B',
				      "BRAVO", "Bob, Example", 9.75);

	madc::error err;
	REQUIRE(ds.insert(alice, &err));
	REQUIRE(ds.insert(bob, &err));

	StorageProbe fetched;
	REQUIRE(ds.get(madc::value(int64_t(2)), fetched, &err));
	CHECK(fetched.id == 2);
	CHECK(fetched.score == 42);
	CHECK(fetched.enabled == false);
	CHECK(short_name_string(fetched) == "BRAVO");
	CHECK(fetched.title == "Bob, Example");
	CHECK(fetched.ratio == doctest::Approx(9.75));

	std::unique_ptr<madc::Cursor<StorageProbe> > filtered =
	    ds.where_eq("short_name", madc::value("ALPHA"), &err);
	REQUIRE(static_cast<bool>(filtered));
	CHECK(count_cursor_rows(std::move(filtered)) == 1);

	bob.score = 77;
	bob.enabled = true;
	bob.title = "Bob Updated";
	bob.ratio = 11.5;
	REQUIRE(ds.update(bob, &err));
	REQUIRE(ds.get(madc::value(int64_t(2)), fetched, &err));
	CHECK(fetched.score == 77);
	CHECK(fetched.enabled == true);
	CHECK(fetched.title == "Bob Updated");
	CHECK(fetched.ratio == doctest::Approx(11.5));

	REQUIRE(ds.erase(madc::value(int64_t(1)), &err));
	std::unique_ptr<madc::Cursor<StorageProbe> > all_rows = ds.scan(&err);
	REQUIRE(static_cast<bool>(all_rows));
	CHECK(count_cursor_rows(std::move(all_rows)) == 1);

	const std::string file_text = read_file(path);
	CHECK(file_text.find("id,score,flags,enabled,status,tag,shortName,title,ratio") != std::string::npos);
	CHECK(file_text.find("\"Bob Updated\"") == std::string::npos);
	CHECK(file_text.find("Bob Updated") != std::string::npos);

	std::remove(path.c_str());
    }

    TEST_CASE("dsv dataset exposes STL-style iteration and container helpers") {
	const std::string path =
	    "/tmp/madc_dsv_iter_" + std::to_string(static_cast<long long>(getpid())) + ".csv";
	std::remove(path.c_str());

	madc::MappingSpec<StorageProbe> spec;
	spec.key("id");

	madc::DataSet<StorageProbe> ds("dsv://" + path);
	ds.mapping(spec).name("users");

	StorageProbe alice = make_probe(1, 10, 7, true, StorageStatus::active, 'A',
					"ALPHA", "Alice Example", 1.25);
	StorageProbe bob = make_probe(2, 42, 9, false, StorageStatus::disabled, 'B',
				      "BRAVO", "Bob Example", 9.75);
	StorageProbe cara = make_probe(3, 77, 11, true, StorageStatus::active, 'C',
				       "CHARLIE", "Cara Example", 7.5);

	madc::error err;
	REQUIRE(ds.empty(&err));
	REQUIRE(ds.insert(alice, &err));
	REQUIRE(ds.push_back(bob, &err));
	REQUIRE(ds.insert(cara, &err));

	CHECK(ds.size(&err) == 3);
	CHECK(ds.count(&err) == 3);
	CHECK(ds.count("enabled", madc::value(true), &err) == 2);
	CHECK_FALSE(ds.empty(&err));
	CHECK(ds.contains(madc::value(int64_t(2)), &err));
	CHECK_FALSE(ds.contains(madc::value(int64_t(99)), &err));

	std::vector<int64_t> ids;
	for (madc::DataSet<StorageProbe>::iterator it = ds.begin(&err);
	     it != ds.end(&err);
	     ++it)
	    ids.push_back(it->id);

	REQUIRE(ids.size() == 3);
	CHECK(ids[0] == 1);
	CHECK(ids[1] == 2);
	CHECK(ids[2] == 3);

	int64_t range_sum = 0;
	for (const auto &row : ds)
	    range_sum += row.id;
	CHECK(range_sum == 6);

	madc::DataSet<StorageProbe>::iterator found = ds.find(madc::value(int64_t(2)), &err);
	REQUIRE(found != ds.end(&err));
	CHECK(found->title == "Bob Example");

	StorageProbe current;
	REQUIRE(ds.current(found, current, &err));
	CHECK(current.id == 2);
	CHECK(current.score == 42);

	REQUIRE(ds.erase(found, &err));
	CHECK(ds.size(&err) == 2);
	CHECK_FALSE(ds.contains(madc::value(int64_t(2)), &err));

	std::vector<int64_t> remaining;
	for (const auto &row : ds)
	    remaining.push_back(row.id);
	REQUIRE(remaining.size() == 2);
	CHECK(remaining[0] == 1);
	CHECK(remaining[1] == 3);

	std::remove(path.c_str());
    }

    TEST_CASE("dsv scans own their file and report row failures during pull") {
	const std::string path =
	    "/tmp/madc_dsv_stream_" + std::to_string(static_cast<long long>(getpid())) + ".csv";
	std::remove(path.c_str());

	{
	    std::ofstream os(path.c_str());
	    os << "id,score,flags,enabled,status,tag,short_name,title,ratio\n";
	    os << "1,10,7,true,1,A,ALPHA,Alice,1.25\n";
	    os << "2,42\n";
	}

	madc::MappingSpec<StorageProbe> spec;
	spec.key("id");
	madc::DataSet<StorageProbe> ds("dsv://" + path);
	ds.mapping(spec).name("users");

	madc::error err;
	REQUIRE(ds.open(&err));
	std::unique_ptr<madc::Cursor<StorageProbe> > cursor = ds.scan(&err);
	REQUIRE(cursor.get() != nullptr);
	ds.close();

	StorageProbe row;
	CHECK(madc::cursor_next(*cursor, row, &err) == madc::CursorStatus::item);
	CHECK(row.id == 1);
	CHECK(madc::cursor_next(*cursor, row, &err) == madc::CursorStatus::failure);
	CHECK(err.message == "dsv row column count does not match header");
	cursor->close();

	std::remove(path.c_str());
    }
}
