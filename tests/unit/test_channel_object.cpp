#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "madcdis/channel.h"

#include <cstdio>
#include <string>
#include <unistd.h>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

namespace {

std::string scratch_uri(const char *tag, std::string &path_out)
{
	path_out = "/tmp/madc_channel_object_" + std::string(tag) + "_"
		+ std::to_string(static_cast<long long>(getpid()));
	return "file://" + path_out;
}

} // namespace

TEST_CASE("channel writes then reads a file with line semantics")
{
	std::string path;
	std::string uri = scratch_uri("lines", path);
	{
		madc::channel writer(uri.c_str(), "w");
		REQUIRE(writer.ok());
		CHECK(std::string(writer.last_error()).empty());
		CHECK(writer.write("alpha\r\nbeta\n"));
		std::string more("gamma\ntail");
		CHECK(writer.write(more));
	}

	madc::channel reader(uri.c_str(), "r");
	REQUIRE(reader.ok());
	std::string line;
	REQUIRE(reader.readline(line));
	CHECK(line == "alpha");
	REQUIRE(reader.readline(line));
	CHECK(line == "beta");
	REQUIRE(reader.readline(line));
	CHECK(line == "gamma");
	REQUIRE(reader.readline(line));
	CHECK(line == "tail");
	CHECK_FALSE(reader.readline(line));
	CHECK(line.empty());

	std::remove(path.c_str());
}

TEST_CASE("channel read serves buffered bytes before the wire")
{
	std::string path;
	std::string uri = scratch_uri("pending", path);
	{
		madc::channel writer(uri.c_str(), "w");
		REQUIRE(writer.ok());
		CHECK(writer.write("abc\ndef\n"));
	}

	madc::channel reader(uri.c_str(), "r");
	std::string line;
	REQUIRE(reader.readline(line));
	CHECK(line == "abc");
	char buffer[2];
	REQUIRE(reader.read(buffer, sizeof(buffer)) == 2);
	CHECK(std::string(buffer, 2) == "de");
	REQUIRE(reader.readline(line));
	CHECK(line == "f");
	CHECK(reader.read(buffer, sizeof(buffer)) == 0);

	std::remove(path.c_str());
}

TEST_CASE("channel readall drains the whole stream")
{
	std::string path;
	std::string uri = scratch_uri("all", path);
	{
		madc::channel writer(uri.c_str(), "w");
		REQUIRE(writer.ok());
		CHECK(writer.write("one\ntwo\nthree"));
	}

	madc::channel reader(uri.c_str(), "r");
	std::string everything;
	REQUIRE(reader.readall(everything));
	CHECK(everything == "one\ntwo\nthree");

	std::remove(path.c_str());
}

TEST_CASE("channel drives an exec:// child end to end")
{
	madc::channel sorter("exec://sort");
	REQUIRE(sorter.ok());
	CHECK(sorter.write("pear\napple\nmango\n"));
	CHECK(sorter.close_write());

	std::string line;
	REQUIRE(sorter.readline(line));
	CHECK(line == "apple");
	REQUIRE(sorter.readline(line));
	CHECK(line == "mango");
	REQUIRE(sorter.readline(line));
	CHECK(line == "pear");
	CHECK_FALSE(sorter.readline(line));
	sorter.close();
	CHECK_FALSE(sorter.ok());
}

TEST_CASE("channel open failures are visible through ok and last_error")
{
	madc::channel missing("exec:///nonexistent/madc-no-such-binary");
	CHECK_FALSE(missing.ok());
	CHECK(std::string(missing.last_error()).find("process exec failed")
	      != std::string::npos);
	std::string line;
	CHECK_FALSE(missing.readline(line));
	char buffer[8];
	CHECK(missing.read(buffer, sizeof(buffer)) == -1);

	madc::channel bad_mode("file:///tmp/whatever", "rb");
	CHECK_FALSE(bad_mode.ok());
	CHECK(std::string(bad_mode.last_error()).find("invalid channel mode")
	      != std::string::npos);
}
