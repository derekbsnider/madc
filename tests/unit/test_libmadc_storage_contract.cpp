// Contract tests for the exploratory libmadc storage/federation API.
//
// These tests intentionally validate API shape and policy defaults
// before backend implementations land.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "libmadc/datasource.h"
#include "libmadc/dataset.h"
#include "libmadc/mapper.h"
#include "libmadc/relation.h"
#include "libmadc/schema.h"

#include <cstdint>
#include <string>

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

TEST_SUITE("libmadc storage contract") {

    TEST_CASE("DataSource parses local file-like schemes as location-only sources") {
	madc::DataSource dsv("dsv:///tmp/users.csv");
	CHECK(dsv.scheme() == "dsv");
	CHECK(dsv.authority().empty());
	CHECK(dsv.path() == "/tmp/users.csv");
	CHECK(dsv.location() == "/tmp/users.csv");
	CHECK(dsv.is_local());
	CHECK_FALSE(dsv.is_remote());

	madc::DataSource flr("flr:///tmp/users.flr");
	CHECK(flr.scheme() == "flr");
	CHECK(flr.path() == "/tmp/users.flr");
	CHECK(flr.is_local());

	madc::DataSource vlr("vlr:///tmp/users.vlr");
	CHECK(vlr.scheme() == "vlr");
	CHECK(vlr.path() == "/tmp/users.vlr");
	CHECK(vlr.is_local());
    }

    TEST_CASE("DataSource parses remote authorities for graph-style sources") {
	madc::DataSource graph("falkordb://127.0.0.1:6379/social");
	CHECK(graph.scheme() == "falkordb");
	CHECK(graph.authority() == "127.0.0.1:6379");
	CHECK(graph.path() == "/social");
	CHECK(graph.location() == "127.0.0.1:6379/social");
	CHECK(graph.is_remote());
	CHECK_FALSE(graph.is_local());
    }

    TEST_CASE("SchemaInfo can describe a mixed storage probe layout") {
	madc::SchemaInfo schema("StorageProbe");
	schema.set_kind(madc::SchemaInfo::kind::structured);
	schema.set_dataset_role(madc::SchemaInfo::role::primary);
	schema.set_packed_binary_compatible(false);
	schema.set_fixed_size(sizeof(StorageProbe));

	madc::SchemaField id;
	id.name = "id";
	id.type_name = "int64_t";
	id.field_kind = madc::SchemaField::kind::integer;
	id.byte_offset = 0;
	id.byte_size = sizeof(int64_t);
	id.is_signed = true;
	id.key = true;
	schema.add_field(id);

	madc::SchemaField short_name;
	short_name.name = "short_name";
	short_name.type_name = "char[16]";
	short_name.field_kind = madc::SchemaField::kind::text;
	short_name.byte_size = 16;
	schema.add_field(short_name);

	madc::SchemaField title;
	title.name = "title";
	title.type_name = "string";
	title.field_kind = madc::SchemaField::kind::text;
	schema.add_field(title);

	REQUIRE(schema.fields().size() == 3);
	CHECK(schema.name() == "StorageProbe");
	CHECK(schema.schema_kind() == madc::SchemaInfo::kind::structured);
	CHECK(schema.dataset_role() == madc::SchemaInfo::role::primary);
	CHECK(schema.fixed_size() == sizeof(StorageProbe));
	CHECK_FALSE(schema.packed_binary_compatible());
	CHECK(schema.fields()[0].key);
	CHECK(schema.fields()[1].byte_size == 16);
	CHECK(schema.fields()[2].field_kind == madc::SchemaField::kind::text);
    }

    TEST_CASE("MappingSpec defaults to logical mapping with strict fixed-record overflow") {
	madc::MappingSpec<StorageProbe> spec;
	CHECK(spec.keys().empty());
	CHECK_FALSE(spec.packed_binary_enabled());
	CHECK_FALSE(spec.document_mode_enabled());
	CHECK(spec.dataset_role() == madc::SchemaInfo::role::primary);
	CHECK(spec.record_layout() == madc::MappingSpec<StorageProbe>::layout_mode::logical);
	CHECK(spec.record_size() == 0);
	CHECK(spec.record_overflow_policy() == madc::MappingSpec<StorageProbe>::overflow_policy::fail);
	CHECK_FALSE(spec.ordered_records());
	CHECK(spec.ordered_key_compare() == madc::SchemaInfo::key_compare::none);
	CHECK(spec.field_names().empty());
	CHECK(spec.excluded_fields().empty());
	CHECK(spec.string_limits().empty());
    }

    TEST_CASE("MappingSpec can express DSV logical mapping with key and field renames") {
	madc::MappingSpec<StorageProbe> spec;
	spec.key("id")
	    .field_name("short_name", "shortName")
	    .field_name("title", "displayTitle");

	REQUIRE(spec.keys().size() == 1);
	CHECK(spec.keys()[0] == "id");
	CHECK(spec.record_layout() == madc::MappingSpec<StorageProbe>::layout_mode::logical);
	CHECK(spec.field_names().at("short_name") == "shortName");
	CHECK(spec.field_names().at("title") == "displayTitle");
    }

    TEST_CASE("MappingSpec can express FLR strict fixed-record policy") {
	madc::MappingSpec<StorageProbe> spec;
	spec.packed_binary(true)
	    .fixed_record_size(64)
	    .role(madc::SchemaInfo::role::index)
	    .ordered_by("id", madc::SchemaInfo::key_compare::numeric_signed);

	CHECK(spec.packed_binary_enabled());
	CHECK(spec.dataset_role() == madc::SchemaInfo::role::index);
	CHECK(spec.record_layout() == madc::MappingSpec<StorageProbe>::layout_mode::fixed_record);
	CHECK(spec.record_size() == 64);
	CHECK(spec.record_overflow_policy() == madc::MappingSpec<StorageProbe>::overflow_policy::fail);
	CHECK(spec.ordered_records());
	CHECK(spec.ordered_key_field() == "id");
	CHECK(spec.ordered_key_compare() == madc::SchemaInfo::key_compare::numeric_signed);
	REQUIRE(spec.keys().size() == 1);
	CHECK(spec.keys()[0] == "id");
    }

    TEST_CASE("MappingSpec only enables truncation when explicitly requested") {
	madc::MappingSpec<StorageProbe> spec;
	spec.fixed_record_size(48,
			       madc::MappingSpec<StorageProbe>::overflow_policy::truncate)
	    .string_field_limit("short_name", 15)
	    .string_field_limit("title",
				31,
				madc::MappingSpec<StorageProbe>::overflow_policy::truncate);

	CHECK(spec.record_layout() == madc::MappingSpec<StorageProbe>::layout_mode::fixed_record);
	CHECK(spec.record_size() == 48);
	CHECK(spec.record_overflow_policy() == madc::MappingSpec<StorageProbe>::overflow_policy::truncate);
	REQUIRE(spec.string_limits().count("short_name") == 1);
	REQUIRE(spec.string_limits().count("title") == 1);
	CHECK(spec.string_limits().at("short_name").max_bytes == 15);
	CHECK(spec.string_limits().at("title").max_bytes == 31);
	CHECK(spec.string_limits().at("title").policy == madc::MappingSpec<StorageProbe>::overflow_policy::truncate);
    }

    TEST_CASE("MappingSpec can express VLR intent for variable-sized records") {
	madc::MappingSpec<StorageProbe> spec;
	spec.variable_record()
	    .role(madc::SchemaInfo::role::payload);

	CHECK(spec.record_layout() == madc::MappingSpec<StorageProbe>::layout_mode::variable_record);
	CHECK(spec.dataset_role() == madc::SchemaInfo::role::payload);
	CHECK(spec.record_size() == 0);
	CHECK(spec.record_overflow_policy() == madc::MappingSpec<StorageProbe>::overflow_policy::fail);
    }

    TEST_CASE("MappingSpec can express append-only VLR locator sidecars") {
	madc::MappingSpec<StorageProbe> spec;
	spec.key("id")
	    .variable_record()
	    .tombstone_file("/tmp/users.vlr.bits")
	    .role(madc::SchemaInfo::role::payload);

	CHECK(spec.record_layout() == madc::MappingSpec<StorageProbe>::layout_mode::variable_record);
	CHECK(spec.has_tombstone_file());
	CHECK(spec.tombstone_path() == "/tmp/users.vlr.bits");
	CHECK(spec.dataset_role() == madc::SchemaInfo::role::payload);
    }

    TEST_CASE("MappingSpec can express FLR tombstone sidecars for pre-reap deletes") {
	madc::MappingSpec<StorageProbe> spec;
	spec.key("id")
	    .fixed_record_size(64)
	    .tombstone_file("/tmp/users.bits")
	    .dead_record_file("/tmp/users.dead");

	CHECK(spec.has_tombstone_file());
	CHECK(spec.tombstone_path() == "/tmp/users.bits");
	CHECK(spec.has_dead_record_file());
	CHECK(spec.dead_record_path() == "/tmp/users.dead");
    }

    TEST_CASE("Relation can express positional offset key and graph bindings") {
	madc::DataSet<StorageProbe> flr("flr:///tmp/users.flr");
	madc::DataSet<StorageProbe> vlr("vlr:///tmp/users.vlr");

	madc::Relation<StorageProbe, StorageProbe> tombstones = madc::relate(flr, flr);
	tombstones.name("deleted_rows")
		  .positional();
	CHECK(tombstones.relation_kind() == madc::RelationKind::positional);
	CHECK(tombstones.keys().empty());

	madc::Relation<StorageProbe, StorageProbe> offsets = madc::relate(flr, vlr);
	offsets.name("payload_offsets")
	       .offset("payload_offset", "record_offset");
	CHECK(offsets.relation_kind() == madc::RelationKind::offset);
	REQUIRE(offsets.keys().size() == 1);
	CHECK(offsets.keys()[0].from_field == "payload_offset");
	CHECK(offsets.keys()[0].to_field == "record_offset");

	madc::Relation<StorageProbe, StorageProbe> keyed = madc::relate(flr, flr);
	keyed.key("id", "id");
	CHECK(keyed.relation_kind() == madc::RelationKind::key_match);
	REQUIRE(keyed.keys().size() == 1);
	CHECK(keyed.keys()[0].from_field == "id");
	CHECK(keyed.keys()[0].to_field == "id");

	madc::Relation<StorageProbe, StorageProbe> graph = madc::relate(flr, flr);
	graph.graph("FRIEND_OF");
	CHECK(graph.relation_kind() == madc::RelationKind::graph_edge);
	CHECK(graph.edge_label() == "FRIEND_OF");
    }
}
