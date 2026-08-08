// The driver capability-truth gate (FLR plan L1.6, wired into fulltest via
// the unit-test suite).
//
// A capability flag is a CONTRACT, not a hope. This gate cross-examines
// advertised DriverCapabilities against observed behavior so an
// interface-without-implementation (the v0.70.0 FLR finding: locator API
// declared, whole-file materialization behind it) fails the build instead
// of surviving until a review.
//
// Truth-leg map — every DriverCapabilities field is accounted for:
//   read / write / scan / point_lookup  behavioral coverage in
//                                       test_libmadc_flr / test_libmadc_vlr /
//                                       dsv storage tests (round-trips, key
//                                       lookups, scans) + the claims table here
//   locator_lookup                      read-counting proof here: a locator hit
//                                       must be positioned access, and the
//                                       claim requires a provable channel seam
//   soft_delete                         erase/restore behavioral tests in
//                                       test_libmadc_flr / vlr suites + claims
//                                       table here
//   range/filter/project/sort/limit/join pushdown, graph_match, edge_expand,
//   path_search, transaction            claimed by NO core driver — the claims
//                                       table pins them false; a driver that
//                                       starts claiming one must land with a
//                                       truth leg and update the table
//
// The static_assert below is the ratchet: adding a capability field changes
// sizeof(DriverCapabilities) and fails this build until the new field has a
// truth leg and the expected size is updated. Do not just bump the number.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "libmadc/datachannel.h"
#include "libmadc/datasource.h"
#include "libmadc/driver.h"
#include "libmadc/error.h"
#include "libmadc/schema.h"
#include "libmadc/value.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

static_assert(sizeof(madc::DriverCapabilities) == 16,
	      "DriverCapabilities changed: add a truth leg for the new "
	      "capability to test_driver_capability_truth.cpp, then update "
	      "the expected size");

namespace {

struct IoCounters
{
	std::size_t reads = 0;
	std::size_t read_bytes = 0;
	std::size_t writes = 0;
	std::size_t write_bytes = 0;

	void reset()
	{
		reads = 0;
		read_bytes = 0;
		writes = 0;
		write_bytes = 0;
	}
};

// The read-counting shim: a real seekable channel whose traffic is visible
// to the test even after the driver takes ownership.
class CountingMemoryChannel : public madc::MemoryDataChannel
{
public:
	explicit CountingMemoryChannel(IoCounters *counters)
		: counters_(counters)
	{}
	CountingMemoryChannel(const std::vector<unsigned char> &seed,
			      IoCounters *counters)
		: madc::MemoryDataChannel(seed), counters_(counters)
	{}

	bool read(void *buffer, std::size_t capacity, std::size_t &bytes_read,
		  madc::error *err = nullptr) override
	{
		bool ok = madc::MemoryDataChannel::read(buffer, capacity,
							bytes_read, err);
		counters_->reads += 1;
		counters_->read_bytes += bytes_read;
		return ok;
	}

	bool read_at(uint64_t offset, void *buffer, std::size_t capacity,
		     std::size_t &bytes_read, madc::error *err = nullptr) override
	{
		bool ok = madc::MemoryDataChannel::read_at(offset, buffer, capacity,
							   bytes_read, err);
		counters_->reads += 1;
		counters_->read_bytes += bytes_read;
		return ok;
	}

	bool write(const void *buffer, std::size_t size,
		   std::size_t &bytes_written, madc::error *err = nullptr) override
	{
		bool ok = madc::MemoryDataChannel::write(buffer, size,
							 bytes_written, err);
		counters_->writes += 1;
		counters_->write_bytes += bytes_written;
		return ok;
	}

	bool write_at(uint64_t offset, const void *buffer, std::size_t size,
		      std::size_t &bytes_written, madc::error *err = nullptr) override
	{
		bool ok = madc::MemoryDataChannel::write_at(offset, buffer, size,
							    bytes_written, err);
		counters_->writes += 1;
		counters_->write_bytes += bytes_written;
		return ok;
	}

private:
	IoCounters *counters_;
};

const std::size_t GATE_RECORD_SIZE = 16;
const std::size_t GATE_RECORD_COUNT = 64;

madc::SchemaInfo gate_schema()
{
	madc::SchemaInfo schema("gate_record");
	schema.set_record_layout(madc::SchemaInfo::layout_mode::fixed_record);
	schema.set_record_size(GATE_RECORD_SIZE);

	madc::SchemaField id;
	id.name = "id";
	id.type_name = "int64_t";
	id.field_kind = madc::SchemaField::kind::integer;
	id.byte_offset = 0;
	id.byte_size = 8;
	id.is_signed = true;
	id.key = true;
	schema.add_field(id);

	madc::SchemaField score;
	score.name = "score";
	score.type_name = "int32_t";
	score.field_kind = madc::SchemaField::kind::integer;
	score.byte_offset = 8;
	score.byte_size = 4;
	score.is_signed = true;
	schema.add_field(score);

	return schema;
}

madc::value gate_record(int64_t id, int64_t score)
{
	madc::value record = madc::value::make_object();
	record.object()["id"] = madc::value(id);
	record.object()["score"] = madc::value(score);
	return record;
}

std::unique_ptr<madc::DataDriver> gate_flr_driver()
{
	return madc::DataDriverRegistry::instance().create(
		madc::DataSource("flr:///capability/gate"));
}

// Open a registry-created flr driver over a counting channel seeded with
// `seed`, returning the raw channel pointer for byte inspection (owned by
// the driver).
CountingMemoryChannel *open_counting_flr(madc::DataDriver &driver,
					 const std::vector<unsigned char> &seed,
					 IoCounters *counters,
					 madc::error *err)
{
	madc::ChannelBackedDataDriver *channel_backed =
		dynamic_cast<madc::ChannelBackedDataDriver *>(&driver);
	REQUIRE(channel_backed != nullptr);
	CountingMemoryChannel *raw = new CountingMemoryChannel(seed, counters);
	std::unique_ptr<madc::DataChannel> channel(raw);
	if ( !channel_backed->open_on_channel(std::move(channel), err) )
		return nullptr;
	return raw;
}

} // namespace

TEST_SUITE("driver capability truth") {

    TEST_CASE("core scheme capability claims match the gate's table") {
	// A claim change is a contract change: it must visit this gate and
	// land with a matching truth leg.
	std::unique_ptr<madc::DataDriver> flr = gate_flr_driver();
	REQUIRE(flr.get() != nullptr);
	madc::DriverCapabilities flr_caps = flr->capabilities();
	CHECK(flr_caps.read);
	CHECK(flr_caps.write);
	CHECK(flr_caps.scan);
	CHECK(flr_caps.point_lookup);
	CHECK(flr_caps.locator_lookup);
	CHECK(flr_caps.soft_delete);
	CHECK_FALSE(flr_caps.range_lookup);
	CHECK_FALSE(flr_caps.filter_pushdown);
	CHECK_FALSE(flr_caps.project_pushdown);
	CHECK_FALSE(flr_caps.sort_pushdown);
	CHECK_FALSE(flr_caps.limit_pushdown);
	CHECK_FALSE(flr_caps.join_pushdown);
	CHECK_FALSE(flr_caps.graph_match);
	CHECK_FALSE(flr_caps.edge_expand);
	CHECK_FALSE(flr_caps.path_search);
	CHECK_FALSE(flr_caps.transaction);

	std::unique_ptr<madc::DataDriver> dsv =
	    madc::DataDriverRegistry::instance().create(
		madc::DataSource("dsv:///capability/gate"));
	REQUIRE(dsv.get() != nullptr);
	madc::DriverCapabilities dsv_caps = dsv->capabilities();
	CHECK(dsv_caps.read);
	CHECK(dsv_caps.write);
	CHECK(dsv_caps.scan);
	CHECK(dsv_caps.point_lookup);
	// dsv has no positioned access story: it must not claim one.
	CHECK_FALSE(dsv_caps.locator_lookup);
	CHECK_FALSE(dsv_caps.soft_delete);

	std::unique_ptr<madc::DataDriver> vlr =
	    madc::DataDriverRegistry::instance().create(
		madc::DataSource("vlr:///capability/gate"));
	REQUIRE(vlr.get() != nullptr);
	madc::DriverCapabilities vlr_caps = vlr->capabilities();
	CHECK(vlr_caps.read);
	CHECK(vlr_caps.write);
	CHECK(vlr_caps.scan);
	CHECK(vlr_caps.point_lookup);
	// vlr serves locators by linear search over materialized rows — an
	// honest false until the S3 offset sidecar makes it positioned.
	CHECK_FALSE(vlr_caps.locator_lookup);
	CHECK(vlr_caps.soft_delete);
    }

    TEST_CASE("a locator_lookup claim requires a provable channel seam") {
	std::vector<std::string> schemes =
	    madc::DataDriverRegistry::instance().schemes();
	REQUIRE(!schemes.empty());
	for ( std::size_t i = 0; i < schemes.size(); ++i )
	{
	    std::unique_ptr<madc::DataDriver> driver =
		madc::DataDriverRegistry::instance().create(
		    madc::DataSource(schemes[i] + ":///capability/gate"));
	    REQUIRE(driver.get() != nullptr);
	    if ( !driver->capabilities().locator_lookup )
		continue;
	    // No injectable channel means no read-counting proof, and an
	    // unprovable claim is a gate failure by design.
	    INFO("scheme `" << schemes[i] << "` claims locator_lookup");
	    CHECK(dynamic_cast<madc::ChannelBackedDataDriver *>(driver.get())
		  != nullptr);
	}
    }

    TEST_CASE("flr locator lookup and positional update are positioned, not scans") {
	madc::error err;
	IoCounters counters;

	std::unique_ptr<madc::DataDriver> writer = gate_flr_driver();
	REQUIRE(writer.get() != nullptr);
	REQUIRE(writer->bind_schema(gate_schema(), &err));
	CountingMemoryChannel *image =
	    open_counting_flr(*writer, std::vector<unsigned char>(),
			      &counters, &err);
	REQUIRE(image != nullptr);
	for ( std::size_t i = 0; i < GATE_RECORD_COUNT; ++i )
	{
	    REQUIRE(writer->insert_record(
			gate_record(static_cast<int64_t>(i),
				    static_cast<int64_t>(i * 10)), &err));
	}
	REQUIRE(image->bytes().size() == GATE_RECORD_COUNT * GATE_RECORD_SIZE);

	// Point lookup by record index: one positioned read, one record.
	counters.reset();
	madc::value out;
	REQUIRE(writer->get_record_by_locator(
		    madc::RecordLocator::at_record_index(37), out, &err));
	CHECK(out.as_object().at("id").as_integer() == 37);
	CHECK(out.as_object().at("score").as_integer() == 370);
	CHECK(counters.reads <= 2);
	CHECK(counters.read_bytes <= 2 * GATE_RECORD_SIZE);

	// A record-aligned byte offset resolves to the same record; a
	// misaligned one is refused.
	counters.reset();
	REQUIRE(writer->get_record_by_locator(
		    madc::RecordLocator::at_byte_offset(37 * GATE_RECORD_SIZE),
		    out, &err));
	CHECK(out.as_object().at("id").as_integer() == 37);
	CHECK(counters.reads <= 2);
	CHECK_FALSE(writer->get_record_by_locator(
			madc::RecordLocator::at_byte_offset(37 * GATE_RECORD_SIZE + 1),
			out, &err));
	CHECK(err.message.find("record-aligned") != std::string::npos);
	CHECK_FALSE(writer->get_record_by_locator(
			madc::RecordLocator::at_record_index(GATE_RECORD_COUNT),
			out, &err));
	CHECK(err.message.find("out of range") != std::string::npos);

	// Positional update: zero reads, one positioned write.
	counters.reset();
	REQUIRE(writer->update_record_by_locator(
		    madc::RecordLocator::at_record_index(37),
		    gate_record(37, 9999), &err));
	CHECK(counters.reads == 0);
	CHECK(counters.writes <= 1);
	CHECK(counters.write_bytes <= GATE_RECORD_SIZE);
	REQUIRE(writer->get_record_by_locator(
		    madc::RecordLocator::at_record_index(37), out, &err));
	CHECK(out.as_object().at("score").as_integer() == 9999);
    }

    TEST_CASE("flr open reads no records and streaming does not drain") {
	madc::error err;

	// Build a record image through the production driver, then measure a
	// FRESH open of it.
	IoCounters build_counters;
	std::unique_ptr<madc::DataDriver> writer = gate_flr_driver();
	REQUIRE(writer.get() != nullptr);
	REQUIRE(writer->bind_schema(gate_schema(), &err));
	CountingMemoryChannel *image =
	    open_counting_flr(*writer, std::vector<unsigned char>(),
			      &build_counters, &err);
	REQUIRE(image != nullptr);
	for ( std::size_t i = 0; i < GATE_RECORD_COUNT; ++i )
	{
	    REQUIRE(writer->insert_record(
			gate_record(static_cast<int64_t>(i),
				    static_cast<int64_t>(i)), &err));
	}
	std::vector<unsigned char> seed = image->bytes();
	writer->close();

	IoCounters counters;
	std::unique_ptr<madc::DataDriver> reader = gate_flr_driver();
	REQUIRE(reader.get() != nullptr);
	REQUIRE(reader->bind_schema(gate_schema(), &err));
	REQUIRE(open_counting_flr(*reader, seed, &counters, &err) != nullptr);

	// Lazy open: geometry only, zero record bytes.
	CHECK(counters.reads == 0);
	CHECK(counters.read_bytes == 0);

	// Streaming: the first row costs one record read; abandoning the
	// cursor after it must not have drained the other 63.
	madc::StreamingDataDriver *streaming =
	    dynamic_cast<madc::StreamingDataDriver *>(reader.get());
	REQUIRE(streaming != nullptr);
	std::unique_ptr<madc::Cursor<madc::value> > cursor =
	    streaming->scan_stream(&err);
	REQUIRE(cursor.get() != nullptr);
	madc::value row;
	REQUIRE(cursor->next(row));
	CHECK(row.as_object().at("id").as_integer() == 0);
	CHECK(counters.reads <= 2);
	CHECK(counters.read_bytes <= 2 * GATE_RECORD_SIZE);
	cursor->close();
	CHECK(counters.reads <= 2);

	// And a full drain visits every record exactly once.
	counters.reset();
	cursor = streaming->scan_stream(&err);
	REQUIRE(cursor.get() != nullptr);
	std::size_t seen = 0;
	while ( cursor->next(row) )
	    ++seen;
	cursor->close();
	CHECK(seen == GATE_RECORD_COUNT);
	CHECK(counters.reads == GATE_RECORD_COUNT);
	CHECK(counters.read_bytes == GATE_RECORD_COUNT * GATE_RECORD_SIZE);
    }

}
