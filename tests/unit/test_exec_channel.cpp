#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "madcdis/datachannel.h"

#include <memory>
#include <string>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

namespace {

std::string drain(madc::DataChannel &channel, madc::error *err)
{
	std::string output;
	char buffer[4096];
	for ( ;; )
	{
		std::size_t count = 0;
		if ( !channel.read(buffer, sizeof(buffer), count, err) )
			return std::string();
		if ( count == 0 )
			return output;
		output.append(buffer, count);
	}
}

std::unique_ptr<madc::DataChannel> open_exec(const std::string &uri,
					     madc::error *err)
{
	return madc::DataChannelRegistry::instance().open(
		madc::DataSource(uri), madc::ChannelOpenMode::read_write, err);
}

} // namespace

TEST_CASE("registry-opened exec://sort round-trips through the child")
{
	madc::error err;
	std::unique_ptr<madc::DataChannel> channel = open_exec("exec://sort", &err);
	REQUIRE(channel.get() != nullptr);
	CHECK(std::string(channel->name()) == "exec");

	madc::ChannelCapabilities capabilities = channel->capabilities();
	CHECK(capabilities.read);
	CHECK(capabilities.write);
	CHECK(capabilities.half_close);
	CHECK_FALSE(capabilities.seek);
	CHECK(madc::seekable_surface(channel.get()) == nullptr);

	// ASCII input so no locale collation can reorder it.
	const char input[] = "pear\napple\nmango\n";
	REQUIRE(madc::write_all(*channel, input, sizeof(input) - 1, &err));
	channel->close_write();
	CHECK_FALSE(channel->capabilities().write);
	CHECK(drain(*channel, &err) == "apple\nmango\npear\n");
	channel->close();
	CHECK_FALSE(channel->capabilities().read);
}

TEST_CASE("exec URI arguments split on single spaces")
{
	madc::error err;
	std::unique_ptr<madc::DataChannel> channel = open_exec("exec://sort -r", &err);
	REQUIRE(channel.get() != nullptr);

	const char input[] = "pear\napple\nmango\n";
	REQUIRE(madc::write_all(*channel, input, sizeof(input) - 1, &err));
	channel->close_write();
	CHECK(drain(*channel, &err) == "pear\nmango\napple\n");
}

TEST_CASE("exec channel spawn failure returns null and reports loudly")
{
	madc::error err;
	std::unique_ptr<madc::DataChannel> channel =
		open_exec("exec:///nonexistent/madc-no-such-binary", &err);
	CHECK(channel.get() == nullptr);
	CHECK(err.message.find("process exec failed") != std::string::npos);
}

TEST_CASE("exec channel requires a command")
{
	madc::error err;
	std::unique_ptr<madc::DataChannel> channel = open_exec("exec://", &err);
	CHECK(channel.get() == nullptr);
	CHECK(!err.message.empty());
}
