#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "libmadc/cursor.h"
#include "libmadc/dataset.h"
#include "libmadc/driver.h"
#include "libmadc/flow.h"
#include "libmadc/sink.h"
#include "madcdis/channel_stream.h"
#include "madcdis/datachannel.h"
#include "madcdis/format_flow.h"
#include "madcdis/query.h"
#include "madcdis/source_adapter.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

namespace {

class LegacyCursor : public madc::Cursor<int> {
public:
	LegacyCursor() : next_(0) {}

	bool next(int &out) override
	{
		if (next_ >= 2)
			return false;
		out = ++next_;
		return true;
	}

	void close() override {}

private:
	int next_;
};

class FailingCursor : public madc::Cursor<int>,
			      public madc::ErrorAwareCursor<int> {
public:
	bool next(int &out) override
	{
		return next_status(out, NULL) == madc::CursorStatus::item;
	}

	madc::CursorStatus next_status(int &, madc::error *err) override
	{
		if (err)
			*err = madc::error(madc::error::severity::error,
					   madc::error::phase::runtime,
					   "cursor failed");
		return madc::CursorStatus::failure;
	}

	void close() override {}
};

class LegacyDriver : public madc::DataDriver {
public:
	const char *name() const override { return "legacy"; }
	const char *scheme() const override { return "legacy"; }
	madc::DriverCapabilities capabilities() const override
	{
		return madc::DriverCapabilities();
	}

	bool bind_schema(const madc::SchemaInfo &, madc::error *) override { return true; }
	bool open(const madc::DataSource &, madc::error *) override { return true; }
	void close() override {}
	bool is_open() const override { return true; }
	bool can_bind_schema(const madc::SchemaInfo &) const override { return true; }
	bool can_execute(const madc::Query &) const override { return false; }

	bool insert_record(const madc::value &, madc::error *) override { return false; }
	bool update_record(const std::string &, const madc::value &,
			   const madc::value &, madc::error *) override
	{
		return false;
	}
	bool erase_record(const std::string &, const madc::value &,
			  madc::error *) override
	{
		return false;
	}
	bool restore_record(const std::string &, const madc::value &,
			    madc::error *) override
	{
		return false;
	}
	bool compact_records(madc::error *) override { return false; }
	bool get_record(const std::string &, const madc::value &,
			madc::value &, madc::error *) const override
	{
		return false;
	}
	bool scan_records(std::vector<madc::value> &out,
			  madc::error *) const override
	{
		out.clear();
		out.push_back(madc::value(int64_t(7)));
		return true;
	}
};

class StreamingDriver : public LegacyDriver,
				public madc::StreamingDataDriver {
public:
	StreamingDriver() : streamed(false) {}

	std::unique_ptr<madc::Cursor<madc::value> >
	scan_stream(madc::error *) const override
	{
		streamed = true;
		std::vector<madc::value> records;
		records.push_back(madc::value(int64_t(9)));
		return std::unique_ptr<madc::Cursor<madc::value> >(
			new madc::detail::VectorCursor<madc::value>(std::move(records)));
	}

	mutable bool streamed;
};

class LegacyFactory : public madc::DataDriverRegistry::Factory {
public:
	std::unique_ptr<madc::DataDriver> create() const override
	{
		return std::unique_ptr<madc::DataDriver>(new LegacyDriver());
	}
};

struct LazyState
{
	LazyState() : pulls(0), closes(0) {}
	std::size_t pulls;
	std::size_t closes;
};

class LazyValueCursor : public madc::Cursor<madc::value> {
public:
	LazyValueCursor(std::shared_ptr<LazyState> state, std::size_t count)
		: state_(state), count_(count), next_(0), closed_(false)
	{}

	~LazyValueCursor() override { close(); }

	bool next(madc::value &out) override
	{
		if ( closed_ || next_ >= count_ )
			return false;
		++state_->pulls;
		out = madc::value(int64_t(++next_));
		return true;
	}

	void close() override
	{
		if ( closed_ )
			return;
		++state_->closes;
		closed_ = true;
	}

private:
	std::shared_ptr<LazyState> state_;
	std::size_t count_;
	std::size_t next_;
	bool closed_;
};

class LazyStreamingDriver : public LegacyDriver,
				    public madc::StreamingDataDriver {
public:
	LazyStreamingDriver(std::shared_ptr<LazyState> state, std::size_t count)
		: state_(state), count_(count)
	{}

	std::unique_ptr<madc::Cursor<madc::value> >
	scan_stream(madc::error *) const override
	{
		return std::unique_ptr<madc::Cursor<madc::value> >(
			new LazyValueCursor(state_, count_));
	}

private:
	std::shared_ptr<LazyState> state_;
	std::size_t count_;
};

class CollectingDriver : public LegacyDriver {
public:
	explicit CollectingDriver(
		std::shared_ptr<std::vector<madc::value> > records)
		: records_(records)
	{}

	bool insert_record(const madc::value &record, madc::error *) override
	{
		records_->push_back(record);
		return true;
	}

private:
	std::shared_ptr<std::vector<madc::value> > records_;
};

class ValueMapper : public madc::DataMapper<madc::value> {
public:
	madc::SchemaInfo schema() const override { return madc::SchemaInfo("values"); }
	madc::value encode(const madc::value &input) const override { return input; }
	madc::value decode(const madc::value &input) const override { return input; }
	const char *name() const override { return "value"; }
};

class PartialWriteChannel : public madc::DataChannel {
public:
	const char *name() const override { return "partial"; }
	madc::ChannelCapabilities capabilities() const override
	{
		madc::ChannelCapabilities caps;
		caps.write = true;
		return caps;
	}
	bool read(void *, std::size_t, std::size_t &bytes_read,
		  madc::error *) override
	{
		bytes_read = 0;
		return false;
	}
	bool write(const void *buffer, std::size_t size, std::size_t &bytes_written,
		   madc::error *) override
	{
		bytes_written = size < 2 ? size : 2;
		const unsigned char *bytes = static_cast<const unsigned char *>(buffer);
		output.insert(output.end(), bytes, bytes + bytes_written);
		return true;
	}
	void close() override {}

	std::vector<unsigned char> output;
};

class FailingChannel : public madc::DataChannel {
public:
	const char *name() const override { return "failing"; }
	madc::ChannelCapabilities capabilities() const override
	{
		madc::ChannelCapabilities caps;
		caps.read = true;
		caps.write = true;
		return caps;
	}
	bool read(void *, std::size_t, std::size_t &bytes_read,
		  madc::error *err) override
	{
		bytes_read = 0;
		if ( err )
			*err = madc::error(madc::error::severity::error,
					   madc::error::phase::runtime,
					   "channel read failed deliberately");
		return false;
	}
	bool write(const void *, std::size_t, std::size_t &bytes_written,
		   madc::error *err) override
	{
		bytes_written = 0;
		if ( err )
			*err = madc::error(madc::error::severity::error,
					   madc::error::phase::runtime,
					   "channel write failed deliberately");
		return false;
	}
	void close() override {}
};

class MemoryChannelFactory : public madc::DataChannelRegistry::Factory {
public:
	std::unique_ptr<madc::DataChannel> open(const madc::DataSource &,
						madc::ChannelOpenMode,
						madc::error *) const override
	{
		return std::unique_ptr<madc::DataChannel>(new madc::MemoryDataChannel());
	}
};

class IntegerLineFormat : public madc::FormatAdapter<int> {
public:
	const char *name() const override { return "integer-lines"; }
	bool read_one(std::istream &input, int &out, madc::error *err) const override
	{
		if ( input >> out )
			return true;
		if ( input.eof() )
			return false;
		if ( err )
			*err = madc::error(madc::error::severity::error,
					   madc::error::phase::runtime,
					   "malformed integer");
		return false;
	}
	bool write_one(std::ostream &output, const int &input,
		       madc::error *) const override
	{
		output << input << '\n';
		return output.good();
	}
};

class EagerSourceAdapter : public madc::SourceAdapter {
public:
	const char *name() const override { return "eager"; }
	bool can_read(const madc::DataSource &) const override { return true; }
	bool discover_types(const madc::DataSource &,
			    std::vector<madc::ExtractedRecordType> &out,
			    madc::error *) const override
	{
		out.push_back(madc::ExtractedRecordType("record"));
		return true;
	}
	bool extract(const madc::DataSource &, const std::string &type_name,
		     std::vector<madc::ExtractedRecord> &out,
		     madc::error *) const override
	{
		madc::ExtractedRecord record;
		record.type_name = type_name;
		record.record = madc::value(int64_t(5));
		out.push_back(record);
		return true;
	}
};

class StreamingSource : public EagerSourceAdapter,
			public madc::StreamingSourceAdapter {
public:
	StreamingSource() : streamed(false) {}

	std::unique_ptr<madc::Cursor<madc::ExtractedRecord> > extract_stream(
		const madc::DataSource &, const std::string &type_name,
		madc::error *) const override
	{
		streamed = true;
		madc::ExtractedRecord record;
		record.type_name = type_name;
		record.record = madc::value(int64_t(9));
		std::vector<madc::ExtractedRecord> records;
		records.push_back(record);
		return std::unique_ptr<madc::Cursor<madc::ExtractedRecord> >(
			new madc::detail::VectorCursor<madc::ExtractedRecord>(
				std::move(records)));
	}

	mutable bool streamed;
};

} // namespace

TEST_CASE("legacy cursor ABI remains source compatible")
{
	LegacyCursor cursor;
	madc::error err;
	int item = 0;

	CHECK(madc::cursor_next(cursor, item, &err) == madc::CursorStatus::item);
	CHECK(item == 1);
	CHECK(madc::cursor_next(cursor, item, &err) == madc::CursorStatus::item);
	CHECK(item == 2);
	CHECK(madc::cursor_next(cursor, item, &err) == madc::CursorStatus::end);
}

TEST_CASE("error-aware cursor distinguishes failure from exhaustion")
{
	FailingCursor cursor;
	madc::error err;
	int item = 0;

	CHECK(madc::cursor_next(cursor, item, &err) == madc::CursorStatus::failure);
	CHECK(err.stage == madc::error::phase::runtime);
	CHECK(err.message == "cursor failed");
}

TEST_CASE("driver adapter preserves legacy materialized scans")
{
	LegacyDriver driver;
	std::unique_ptr<madc::Cursor<madc::value> > cursor =
		madc::scan_driver_cursor(driver, NULL);
	madc::value record;

	REQUIRE(cursor.get() != nullptr);
	CHECK(cursor->next(record));
	CHECK(record.as_integer() == 7);
}

TEST_CASE("driver adapter selects optional streaming extension")
{
	StreamingDriver driver;
	std::unique_ptr<madc::Cursor<madc::value> > cursor =
		madc::scan_driver_cursor(driver, NULL);
	madc::value record;

	REQUIRE(cursor.get() != nullptr);
	CHECK(driver.streamed);
	CHECK(cursor->next(record));
	CHECK(record.as_integer() == 9);
}

TEST_CASE("driver registry remains available in the core library")
{
	madc::DataDriverRegistry &registry = madc::DataDriverRegistry::instance();
	registry.register_factory("contract",
		std::unique_ptr<madc::DataDriverRegistry::Factory>(new LegacyFactory()));
	std::unique_ptr<madc::DataDriver> driver =
		registry.create(madc::DataSource("contract://unit"));
	REQUIRE(driver.get() != nullptr);
	CHECK(std::string(driver->name()) == "legacy");
}

TEST_CASE("dataset scan pulls and decodes one row at a time")
{
	std::shared_ptr<LazyState> state(new LazyState());
	madc::DataSet<madc::value> dataset("lazy://unit");
	dataset.mapper(std::shared_ptr<madc::DataMapper<madc::value> >(new ValueMapper()));
	dataset.driver(std::unique_ptr<madc::DataDriver>(new LazyStreamingDriver(state, 100)));

	std::unique_ptr<madc::Cursor<madc::value> > cursor = dataset.scan();
	REQUIRE(cursor.get() != nullptr);
	CHECK(state->pulls == 0);

	madc::value row;
	CHECK(cursor->next(row));
	CHECK(row.as_integer() == 1);
	CHECK(state->pulls == 1);
	cursor->close();
	CHECK(state->closes == 1);
}

TEST_CASE("dataset local query limit does not drain a streaming driver")
{
	std::shared_ptr<LazyState> state(new LazyState());
	madc::DataSet<madc::value> dataset("lazy://unit");
	dataset.mapper(std::shared_ptr<madc::DataMapper<madc::value> >(new ValueMapper()));
	dataset.driver(std::unique_ptr<madc::DataDriver>(new LazyStreamingDriver(state, 100)));

	std::unique_ptr<madc::Cursor<madc::value> > cursor =
		dataset.query(madc::query().limit(1).build());
	REQUIRE(cursor.get() != nullptr);
	CHECK(state->pulls == 0);

	madc::value row;
	CHECK(cursor->next(row));
	CHECK(row.as_integer() == 1);
	CHECK_FALSE(cursor->next(row));
	CHECK(state->pulls == 1);
}

TEST_CASE("filter is lazy and pulls only until the next match")
{
	std::size_t pulls = 0;
	std::unique_ptr<madc::Cursor<int> > generated = madc::from_generator<int>(
		std::function<bool(int &, madc::error *)>(
			[&pulls](int &out, madc::error *) {
				if ( pulls >= 4 )
					return false;
				out = static_cast<int>(++pulls);
				return true;
			}));
	std::unique_ptr<madc::Cursor<int> > even = madc::filter<int>(
		std::move(generated), [](int input) { return input % 2 == 0; });

	CHECK(pulls == 0);
	int item = 0;
	CHECK(even->next(item));
	CHECK(item == 2);
	CHECK(pulls == 2);
}

TEST_CASE("filter transform and copy stream through a container sink")
{
	std::vector<int> input;
	input.push_back(1);
	input.push_back(2);
	input.push_back(3);
	input.push_back(4);
	std::unique_ptr<madc::Cursor<int> > source(
		new madc::detail::VectorCursor<int>(input));
	std::unique_ptr<madc::Cursor<int> > even = madc::filter<int>(
		std::move(source), [](int item) { return item % 2 == 0; });
	std::unique_ptr<madc::Cursor<std::string> > rendered =
		madc::transform<int, std::string>(
			std::move(even),
			[](int item) { return std::string("item-") + std::to_string(item); });

	std::vector<std::string> output;
	madc::BackInsertSink<std::vector<std::string> > sink = madc::to_container(output);
	CHECK(madc::copy(std::move(rendered), sink));
	REQUIRE(output.size() == 2);
	CHECK(output[0] == "item-2");
	CHECK(output[1] == "item-4");
}

TEST_CASE("downstream failure closes the upstream cursor")
{
	std::shared_ptr<LazyState> state(new LazyState());
	std::unique_ptr<madc::Cursor<madc::value> > source(
		new LazyValueCursor(state, 10));
	madc::FunctionSink<madc::value> sink = madc::to_function<madc::value>(
		[](const madc::value &, madc::error *err) {
			if ( err )
				*err = madc::error(madc::error::severity::error,
						   madc::error::phase::runtime,
						   "sink failed");
			return false;
		});
	madc::error err;

	CHECK_FALSE(madc::copy(std::move(source), sink, &err));
	CHECK(err.message == "sink failed");
	CHECK(state->pulls == 1);
	CHECK(state->closes == 1);
}

TEST_CASE("dataset cursors flow lazily through transforms into dataset sinks")
{
	std::shared_ptr<LazyState> source_state(new LazyState());
	madc::DataSet<madc::value> source("lazy://source");
	source.mapper(std::shared_ptr<madc::DataMapper<madc::value> >(new ValueMapper()));
	source.driver(std::unique_ptr<madc::DataDriver>(
		new LazyStreamingDriver(source_state, 4)));

	std::shared_ptr<std::vector<madc::value> > inserted(
		new std::vector<madc::value>());
	madc::DataSet<madc::value> destination("collect://destination");
	destination.mapper(
		std::shared_ptr<madc::DataMapper<madc::value> >(new ValueMapper()));
	destination.driver(std::unique_ptr<madc::DataDriver>(
		new CollectingDriver(inserted)));

	std::unique_ptr<madc::Cursor<madc::value> > odd = madc::filter<madc::value>(
		source.scan(), [](const madc::value &item) {
			return item.as_integer() % 2 != 0;
		});
	std::unique_ptr<madc::Cursor<madc::value> > scaled =
		madc::transform<madc::value, madc::value>(
			std::move(odd), [](const madc::value &item) {
				return madc::value(item.as_integer() * 10);
			});
	madc::DataSetSink<madc::value> sink(destination);

	REQUIRE(madc::copy(std::move(scaled), sink));
	CHECK(source_state->pulls == 4);
	REQUIRE(inserted->size() == 2);
	CHECK((*inserted)[0].as_integer() == 10);
	CHECK((*inserted)[1].as_integer() == 30);
}

TEST_CASE("memory channel preserves binary data and distinguishes EOF")
{
	std::vector<unsigned char> seed;
	seed.push_back(static_cast<unsigned char>('a'));
	seed.push_back(0);
	seed.push_back(static_cast<unsigned char>('b'));
	madc::MemoryDataChannel channel(seed);
	unsigned char buffer[2] = {0, 0};
	std::size_t count = 0;

	CHECK(channel.read(buffer, sizeof(buffer), count));
	REQUIRE(count == 2);
	CHECK(buffer[0] == static_cast<unsigned char>('a'));
	CHECK(buffer[1] == 0);
	CHECK(channel.read(buffer, sizeof(buffer), count));
	CHECK(count == 1);
	CHECK(buffer[0] == static_cast<unsigned char>('b'));
	CHECK(channel.read(buffer, sizeof(buffer), count));
	CHECK(count == 0);
}

TEST_CASE("write_all handles partial channel writes")
{
	PartialWriteChannel channel;
	const unsigned char input[] = {'a', 0, 'b', 'c', 'd'};

	CHECK(madc::write_all(channel, input, sizeof(input)));
	REQUIRE(channel.output.size() == sizeof(input));
	CHECK(std::memcmp(&channel.output[0], input, sizeof(input)) == 0);
}

TEST_CASE("file channel roundtrips bytes through the core registry")
{
	const std::string path = "/tmp/madc_data_channel_contract_"
		+ std::to_string(static_cast<long long>(getpid())) + ".bin";
	const std::string uri = std::string("file://") + path;
	const unsigned char input[] = {'x', 0, 'y'};
	madc::error err;

	std::unique_ptr<madc::DataChannel> output =
		madc::DataChannelRegistry::instance().open(
			madc::DataSource(uri), madc::ChannelOpenMode::write, &err);
	REQUIRE(output.get() != nullptr);
	CHECK(madc::write_all(*output, input, sizeof(input), &err));
	output->close();

	std::unique_ptr<madc::DataChannel> input_channel =
		madc::DataChannelRegistry::instance().open(
			madc::DataSource(uri), madc::ChannelOpenMode::read, &err);
	REQUIRE(input_channel.get() != nullptr);
	PartialWriteChannel collected;
	CHECK(madc::copy_channel(*input_channel, collected, &err));
	REQUIRE(collected.output.size() == sizeof(input));
	CHECK(std::memcmp(&collected.output[0], input, sizeof(input)) == 0);
	input_channel->close();
	std::remove(path.c_str());
}

TEST_CASE("FIFO channel reports a closed reader without SIGPIPE termination")
{
	const std::string path = "/tmp/madc_data_channel_fifo_"
		+ std::to_string(static_cast<long long>(getpid()));
	const std::string uri = std::string("pipe://") + path;
	madc::error err;

	REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
	int reader = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
	REQUIRE(reader >= 0);
	std::unique_ptr<madc::DataChannel> output =
		madc::DataChannelRegistry::instance().open(
			madc::DataSource(uri), madc::ChannelOpenMode::write, &err);
	REQUIRE(output.get() != nullptr);
	::close(reader);

	const unsigned char byte = 'x';
	std::size_t written = 1;
	CHECK_FALSE(output->write(&byte, 1, written, &err));
	CHECK(written == 0);
	CHECK(err.stage == madc::error::phase::runtime);
	CHECK(err.message.find("Broken pipe") != std::string::npos);

	output->close();
	std::remove(path.c_str());
}

TEST_CASE("channel and driver registries may share a scheme")
{
	madc::DataDriverRegistry::instance().register_factory(
		"contract",
		std::unique_ptr<madc::DataDriverRegistry::Factory>(new LegacyFactory()));
	madc::DataChannelRegistry::instance().register_factory(
		"contract",
		std::unique_ptr<madc::DataChannelRegistry::Factory>(
			new MemoryChannelFactory()));

	CHECK(madc::DataDriverRegistry::instance().has_factory("contract"));
	CHECK(madc::DataChannelRegistry::instance().has_factory("contract"));
}

TEST_CASE("format adapters stream typed values over channels")
{
	const char encoded[] = "10\n20\n";
	std::vector<unsigned char> seed(encoded, encoded + sizeof(encoded) - 1);
	madc::MemoryDataChannel input_channel(seed);
	madc::ChannelInputStream input(input_channel);
	IntegerLineFormat format;
	madc::FormatCursor<int> cursor(input, format);
	int item = 0;

	CHECK(cursor.next(item));
	CHECK(item == 10);
	CHECK(cursor.next(item));
	CHECK(item == 20);
	CHECK_FALSE(cursor.next(item));

	madc::MemoryDataChannel output_channel;
	madc::ChannelOutputStream output(output_channel);
	madc::FormatSink<int> sink(output, format);
	CHECK(sink.put(30));
	CHECK(sink.put(40));
	CHECK(sink.close());
	std::string result(output_channel.bytes().begin(), output_channel.bytes().end());
	CHECK(result == "30\n40\n");
}

TEST_CASE("format adapters preserve channel failures as errors")
{
	IntegerLineFormat format;
	madc::error err;

	FailingChannel read_channel;
	madc::ChannelInputStream input(read_channel);
	madc::FormatCursor<int> cursor(input, format);
	int item = 0;
	CHECK(madc::cursor_next(cursor, item, &err) == madc::CursorStatus::failure);
	CHECK(err.message == "channel read failed deliberately");

	FailingChannel write_channel;
	madc::ChannelOutputStream output(write_channel);
	madc::FormatSink<int> sink(output, format);
	err = madc::error();
	CHECK_FALSE(sink.put(42, &err));
	CHECK(err.message == "channel write failed deliberately");
}

TEST_CASE("source adapter cursor preserves eager implementations")
{
	EagerSourceAdapter adapter;
	std::unique_ptr<madc::Cursor<madc::ExtractedRecord> > cursor =
		madc::extract_adapter_cursor(
			adapter, madc::DataSource("file:///tmp/input"), "record");
	madc::ExtractedRecord record;

	REQUIRE(cursor.get() != nullptr);
	CHECK(cursor->next(record));
	CHECK(record.record.as_integer() == 5);
}

TEST_CASE("source adapter cursor selects the streaming extension")
{
	StreamingSource adapter;
	std::unique_ptr<madc::Cursor<madc::ExtractedRecord> > cursor =
		madc::extract_adapter_cursor(
			adapter, madc::DataSource("file:///tmp/input"), "record");
	madc::ExtractedRecord record;

	REQUIRE(cursor.get() != nullptr);
	CHECK(adapter.streamed);
	CHECK(cursor->next(record));
	CHECK(record.record.as_integer() == 9);
}
