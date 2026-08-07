#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "libmadc/cursor.h"
#include "libmadc/dataset.h"
#include "libmadc/driver.h"
#include "libmadc/flow.h"
#include "libmadc/sink.h"
#include "madcdis/query.h"

#include <string>
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

class ValueMapper : public madc::DataMapper<madc::value> {
public:
	madc::SchemaInfo schema() const override { return madc::SchemaInfo("values"); }
	madc::value encode(const madc::value &input) const override { return input; }
	madc::value decode(const madc::value &input) const override { return input; }
	const char *name() const override { return "value"; }
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
