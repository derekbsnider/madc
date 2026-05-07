// Unit tests for the first concrete cross-dataset storage relation:
// FLR index rows pointing at VLR payload rows by byte offset.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "libmadc/dataset.h"
#include "libmadc/relation.h"

#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <unistd.h>

struct PayloadRecord
{
    int64_t payload_id;
    std::string body;
    int32_t priority;
};

struct PayloadIndexRecord
{
    int64_t id;
    int64_t payload_offset;
    char short_name[16];
};

struct PayloadNoteRecord
{
    int64_t id;
    std::string note;
};

namespace {

void assign_short_name(PayloadIndexRecord &row, const std::string &name)
{
    std::size_t i = 0;
    for ( ; i + 1 < sizeof(row.short_name) && i < name.size(); ++i )
	row.short_name[i] = name[i];
    row.short_name[i] = '\0';
    for ( ++i; i < sizeof(row.short_name); ++i )
	row.short_name[i] = '\0';
}

PayloadRecord make_payload(int64_t id, const std::string &body, int32_t priority)
{
    PayloadRecord out;
    out.payload_id = id;
    out.body = body;
    out.priority = priority;
    return out;
}

PayloadIndexRecord make_index(int64_t id,
			      int64_t payload_offset,
			      const std::string &short_name)
{
    PayloadIndexRecord out;
    out.id = id;
    out.payload_offset = payload_offset;
    assign_short_name(out, short_name);
    return out;
}

} // namespace

namespace madc {

template <>
struct MapperRegistration<PayloadRecord>
{
    static std::shared_ptr<DataMapper<PayloadRecord> > make(const MappingSpec<PayloadRecord> &spec)
    {
	(void)spec;
	MapperBuilder<PayloadRecord> builder("PayloadRecord");
	builder.field("payload_id", &PayloadRecord::payload_id, true);
	builder.field("body", &PayloadRecord::body);
	builder.field("priority", &PayloadRecord::priority);
	return builder.build();
    }
};

template <>
struct MapperRegistration<PayloadIndexRecord>
{
    static std::shared_ptr<DataMapper<PayloadIndexRecord> > make(const MappingSpec<PayloadIndexRecord> &spec)
    {
	(void)spec;
	MapperBuilder<PayloadIndexRecord> builder("PayloadIndexRecord");
	builder.field("id", &PayloadIndexRecord::id, true, 0);
	builder.field("payload_offset", &PayloadIndexRecord::payload_offset, false, 8);
	builder.field("short_name", &PayloadIndexRecord::short_name, false, 16);
	return builder.build();
    }
};

template <>
struct MapperRegistration<PayloadNoteRecord>
{
    static std::shared_ptr<DataMapper<PayloadNoteRecord> > make(const MappingSpec<PayloadNoteRecord> &spec)
    {
	(void)spec;
	MapperBuilder<PayloadNoteRecord> builder("PayloadNoteRecord");
	builder.field("id", &PayloadNoteRecord::id, true);
	builder.field("note", &PayloadNoteRecord::note);
	return builder.build();
    }
};

} // namespace madc

TEST_SUITE("libmadc relation backend") {

    TEST_CASE("flr offset relation resolves vlr payload rows by byte locator") {
	const std::string index_path =
	    "/tmp/madc_relation_index_" + std::to_string(static_cast<long long>(getpid())) + ".flr";
	const std::string payload_path =
	    "/tmp/madc_relation_payload_" + std::to_string(static_cast<long long>(getpid())) + ".vlr";
	const std::string payload_tombstones =
	    "/tmp/madc_relation_payload_" + std::to_string(static_cast<long long>(getpid())) + ".bits";
	std::remove(index_path.c_str());
	std::remove(payload_path.c_str());
	std::remove(payload_tombstones.c_str());

	madc::MappingSpec<PayloadIndexRecord> index_spec;
	index_spec.key("id")
		  .fixed_record_size(32)
		  .ordered_by("id", madc::SchemaInfo::key_compare::numeric_signed)
		  .role(madc::SchemaInfo::role::index);

	madc::MappingSpec<PayloadRecord> payload_spec;
	payload_spec.key("payload_id")
		   .variable_record()
		   .tombstone_file(payload_tombstones)
		   .role(madc::SchemaInfo::role::payload);

	madc::DataSet<PayloadIndexRecord> index("flr://" + index_path);
	index.mapping(index_spec).name("payload_index");
	madc::DataSet<PayloadRecord> payloads("vlr://" + payload_path);
	payloads.mapping(payload_spec).name("payload_rows");

	PayloadRecord alpha = make_payload(101, "This is the first payload body.", 7);
	PayloadRecord beta = make_payload(202, "Second payload body with more text.", 11);

	madc::RecordLocator alpha_loc;
	madc::RecordLocator beta_loc;
	madc::error err;
	REQUIRE(payloads.insert_with_locator(alpha, alpha_loc, &err));
	REQUIRE(payloads.insert_with_locator(beta, beta_loc, &err));
	CHECK(alpha_loc.valid());
	CHECK(beta_loc.valid());
	CHECK(alpha_loc.locator_kind == madc::RecordLocator::kind::byte_offset);
	CHECK(beta_loc.locator_kind == madc::RecordLocator::kind::byte_offset);
	CHECK(beta_loc.byte_offset > alpha_loc.byte_offset);

	REQUIRE(index.insert(make_index(1, static_cast<int64_t>(alpha_loc.byte_offset), "ALPHA"), &err));
	REQUIRE(index.insert(make_index(2, static_cast<int64_t>(beta_loc.byte_offset), "BRAVO"), &err));

	PayloadRecord direct_fetch;
	REQUIRE(payloads.get_by_locator(beta_loc, direct_fetch, &err));
	CHECK(direct_fetch.payload_id == 202);
	CHECK(direct_fetch.body == "Second payload body with more text.");
	CHECK(direct_fetch.priority == 11);

	madc::Relation<PayloadIndexRecord, PayloadRecord> payload_relation =
	    madc::relate(index, payloads);
	payload_relation.name("payload_offsets")
			.offset("payload_offset", "record_offset");

	PayloadRecord resolved;
	REQUIRE(payload_relation.resolve(madc::value(int64_t(1)), resolved, &err));
	CHECK(resolved.payload_id == 101);
	CHECK(resolved.body == "This is the first payload body.");
	CHECK(resolved.priority == 7);

	REQUIRE(payload_relation.resolve(madc::value(int64_t(2)), resolved, &err));
	CHECK(resolved.payload_id == 202);
	CHECK(resolved.body == "Second payload body with more text.");
	CHECK(resolved.priority == 11);

	std::unique_ptr<madc::Cursor<PayloadRecord> > related =
	    payload_relation.query_related(
		madc::query().from("payload_index").where_gte("id", madc::value(int64_t(2))).build(),
		madc::query().from("payload_rows").where_eq("payload_id", madc::value(int64_t(202))).limit(1).build(),
		&err);
	REQUIRE(static_cast<bool>(related));
	REQUIRE(related->next(resolved));
	CHECK(resolved.payload_id == 202);
	CHECK(resolved.body == "Second payload body with more text.");
	CHECK_FALSE(related->next(resolved));
	related->close();

	std::unique_ptr<madc::Cursor<madc::value> > projected =
	    payload_relation.query_related_raw(
		madc::query().from("payload_index").where_gte("id", madc::value(int64_t(2))).build(),
		madc::query().from("payload_rows")
		    .where_gt("payload_id", madc::value(int64_t(150)))
		    .select(std::vector<std::string>{"payload_id", "body"})
		    .limit(1)
		    .build(),
		&err);
	REQUIRE(static_cast<bool>(projected));
	madc::value projected_row;
	REQUIRE(projected->next(projected_row));
	REQUIRE(projected_row.is_object());
	CHECK(projected_row.as_object().count("payload_id") == 1);
	CHECK(projected_row.as_object().count("body") == 1);
	CHECK(projected_row.as_object().count("priority") == 0);
	CHECK(projected_row.as_object().at("payload_id") == madc::value(int64_t(202)));
	CHECK(projected_row.as_object().at("body") == madc::value("Second payload body with more text."));
	CHECK_FALSE(projected->next(projected_row));
	projected->close();

	std::unique_ptr<madc::Cursor<madc::value> > full_rows =
	    payload_relation.query_related_raw(
		madc::query().from("payload_index").where_eq("id", madc::value(int64_t(1))).build(),
		madc::query().from("payload_rows").build(),
		&err);
	REQUIRE(static_cast<bool>(full_rows));
	REQUIRE(full_rows->next(projected_row));
	REQUIRE(projected_row.is_object());
	CHECK(projected_row.as_object().count("payload_id") == 1);
	CHECK(projected_row.as_object().count("body") == 1);
	CHECK(projected_row.as_object().count("priority") == 1);
	CHECK(projected_row.as_object().at("payload_id") == madc::value(int64_t(101)));
	CHECK(projected_row.as_object().at("priority") == madc::value(int64_t(7)));
	CHECK_FALSE(full_rows->next(projected_row));
	full_rows->close();

	std::remove(index_path.c_str());
	std::remove(payload_path.c_str());
	std::remove(payload_tombstones.c_str());
    }

#if defined(HAVE_SQLITE3) && defined(HAVE_QDBM)
    TEST_CASE("key-match relation can traverse filtered source rows across backends") {
	const std::string index_path =
	    "/tmp/madc_relation_users_" + std::to_string(static_cast<long long>(getpid())) + ".db";
	const std::string note_path =
	    "/tmp/madc_relation_notes_" + std::to_string(static_cast<long long>(getpid())) + ".villa";
	std::remove(index_path.c_str());
	std::remove(note_path.c_str());

	madc::MappingSpec<PayloadIndexRecord> index_spec;
	index_spec.key("id");

	madc::MappingSpec<PayloadNoteRecord> note_spec;
	note_spec.key("id");

	madc::DataSet<PayloadIndexRecord> users("sqlite://" + index_path);
	users.mapping(index_spec).name("users");

	madc::DataSet<PayloadNoteRecord> notes("qdbm://" + note_path);
	notes.mapping(note_spec).name("notes");

	madc::error err;
	REQUIRE(users.insert(make_index(1, 0, "ALPHA"), &err));
	REQUIRE(users.insert(make_index(2, 0, "BRAVO"), &err));
	REQUIRE(users.insert(make_index(3, 0, "CHARLIE"), &err));

	REQUIRE(notes.insert(PayloadNoteRecord{1, "alpha note"}, &err));
	REQUIRE(notes.insert(PayloadNoteRecord{2, "bravo note"}, &err));
	REQUIRE(notes.insert(PayloadNoteRecord{3, "charlie note"}, &err));

	madc::Relation<PayloadIndexRecord, PayloadNoteRecord> user_notes =
	    madc::relate(users, notes);
	user_notes.name("user_notes")
		  .key("id", "id");

	std::unique_ptr<madc::Cursor<PayloadNoteRecord> > related =
	    user_notes.query_related(
		madc::query().from("users").where_gte("id", madc::value(int64_t(2))).limit(2).build(),
		madc::query().from("notes").where_eq("note", madc::value("charlie note")).limit(1).build(),
		&err);
	REQUIRE(static_cast<bool>(related));

	PayloadNoteRecord note;
	REQUIRE(related->next(note));
	CHECK(note.id == 3);
	CHECK(note.note == "charlie note");
	CHECK_FALSE(related->next(note));
	related->close();

	std::unique_ptr<madc::Cursor<madc::value> > projected =
	    user_notes.query_related_raw(
		madc::query().from("users").where_gte("id", madc::value(int64_t(2))).limit(2).build(),
		madc::query().from("notes")
		    .where_eq("note", madc::value("charlie note"))
		    .select(std::vector<std::string>{"note"})
		    .limit(1)
		    .build(),
		&err);
	REQUIRE(static_cast<bool>(projected));

	madc::value projected_row;
	REQUIRE(projected->next(projected_row));
	REQUIRE(projected_row.is_object());
	CHECK(projected_row.as_object().count("note") == 1);
	CHECK(projected_row.as_object().count("id") == 0);
	CHECK(projected_row.as_object().at("note") == madc::value("charlie note"));
	CHECK_FALSE(projected->next(projected_row));
	projected->close();

	std::remove(index_path.c_str());
	std::remove(note_path.c_str());
    }
#endif
}
