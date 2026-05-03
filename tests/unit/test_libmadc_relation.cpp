// Unit tests for the first concrete cross-dataset storage relation:
// FLR index rows pointing at VLR payload rows by byte offset.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

bool madc_verbose = false;
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

} // namespace madc

TEST_SUITE("libmadc relation backend") {

    TEST_CASE("flr offset relation resolves vlr payload rows by byte locator") {
	const std::string index_path =
	    "/tmp/madc_relation_index_" + std::to_string(static_cast<long long>(getpid())) + ".flr";
	const std::string payload_path =
	    "/tmp/madc_relation_payload_" + std::to_string(static_cast<long long>(getpid())) + ".vlr";
	std::remove(index_path.c_str());
	std::remove(payload_path.c_str());

	madc::MappingSpec<PayloadIndexRecord> index_spec;
	index_spec.key("id")
		  .fixed_record_size(32)
		  .ordered_by("id", madc::SchemaInfo::key_compare::numeric_signed)
		  .role(madc::SchemaInfo::role::index);

	madc::MappingSpec<PayloadRecord> payload_spec;
	payload_spec.key("payload_id")
		   .variable_record()
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

	std::remove(index_path.c_str());
	std::remove(payload_path.c_str());
    }
}
