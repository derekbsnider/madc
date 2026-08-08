#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "madcdis/channel_stream.h"
#include "madcdis/format_flow.h"
#include "madcdis/process.h"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <string>
#include <unistd.h>
#include <vector>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

namespace {

class LineFormat : public madc::FormatAdapter<std::string> {
public:
	const char *name() const override { return "line"; }

	bool read_one(std::istream &input, std::string &out,
		      madc::error *) const override
	{
		return static_cast<bool>(std::getline(input, out));
	}

	bool write_one(std::ostream &output, const std::string &input,
		       madc::error *) const override
	{
		output << input << '\n';
		return output.good();
	}
};

std::string fixture_path()
{
	char path[4096];
	ssize_t count = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
	if ( count <= 0 )
		return std::string();
	path[count] = '\0';
	std::string executable(path);
	std::size_t slash = executable.find_last_of('/');
	return executable.substr(0, slash + 1) + "test_process_fixture";
}

std::vector<unsigned char> read_channel(madc::DataChannel &channel,
					madc::error *err)
{
	std::vector<unsigned char> output;
	unsigned char buffer[4096];
	for ( ;; )
	{
		std::size_t count = 0;
		if ( !channel.read(buffer, sizeof(buffer), count, err) )
			return std::vector<unsigned char>();
		if ( count == 0 )
			return output;
		output.insert(output.end(), buffer, buffer + count);
	}
}

std::string as_string(const std::vector<unsigned char> &bytes)
{
	return std::string(bytes.begin(), bytes.end());
}

std::size_t open_fd_count()
{
	DIR *directory = ::opendir("/proc/self/fd");
	if ( !directory )
		return 0;
	std::size_t count = 0;
	while ( ::readdir(directory) )
		++count;
	::closedir(directory);
	return count;
}

} // namespace

TEST_CASE("DataSource classifies process and reserved mail schemes")
{
	madc::DataSource process("exec:///opt/madc/filter");
	CHECK(process.source_domain() == madc::DataSource::domain::execution);
	CHECK(process.source_family() == madc::DataSource::family::process);
	CHECK(process.is_process());
	CHECK(process.is_local());
	CHECK(process.path() == "/opt/madc/filter");

	madc::DataSource mail("imaps://mail.example.test/inbox");
	CHECK(mail.source_domain() == madc::DataSource::domain::service);
	CHECK(mail.source_family() == madc::DataSource::family::service_api);
	CHECK(mail.is_remote());

	madc::DataSource plain("tmp/input.bin");
	CHECK(plain.is_plain_file());
	CHECK(plain.is_local());
}

TEST_CASE("Process preserves argv boundaries without invoking a shell")
{
	madc::ProcessOptions options;
	options.args.push_back("argv");
	options.args.push_back("alpha beta");
	options.args.push_back("$(printf not-a-shell)");
	madc::Process process(madc::DataSource("exec://" + fixture_path()), options);
	madc::error err;

	REQUIRE(process.start(&err));
	REQUIRE(process.close_stdin(&err));
	std::vector<unsigned char> stdout_bytes = read_channel(process.stdout_channel(), &err);
	std::vector<unsigned char> stderr_bytes = read_channel(process.stderr_channel(), &err);
	REQUIRE(process.wait(&err));

	CHECK(as_string(stdout_bytes)
	      == "10:alpha beta\n21:$(printf not-a-shell)\n");
	CHECK(stderr_bytes.empty());
	CHECK(process.exited());
	CHECK(process.exit_status() == 0);
}

TEST_CASE("Process keeps stdout stderr and exit status independent")
{
	madc::ProcessOptions options;
	options.args.push_back("echo");
	options.args.push_back("diagnostic");
	options.args.push_back("7");
	madc::Process process(madc::DataSource("exec://" + fixture_path()), options);
	madc::error err;
	const char input[] = { 'a', '\0', 'b', '\n' };

	REQUIRE(process.start(&err));
	REQUIRE(madc::write_all(process.stdin_channel(), input, sizeof(input), &err));
	REQUIRE(process.close_stdin(&err));
	std::vector<unsigned char> stdout_bytes = read_channel(process.stdout_channel(), &err);
	std::vector<unsigned char> stderr_bytes = read_channel(process.stderr_channel(), &err);
	REQUIRE(process.wait(&err));

	REQUIRE(stdout_bytes.size() == sizeof(input));
	CHECK(std::equal(stdout_bytes.begin(), stdout_bytes.end(), input));
	CHECK(as_string(stderr_bytes) == "diagnostic");
	CHECK(process.exit_status() == 7);
}

TEST_CASE("process pump concurrently drains large stdout and stderr")
{
	std::vector<unsigned char> input_bytes(1024 * 1024);
	for ( std::size_t i = 0; i < input_bytes.size(); ++i )
		input_bytes[i] = static_cast<unsigned char>(i % 251);

	madc::MemoryDataChannel input(input_bytes);
	madc::MemoryDataChannel output;
	madc::MemoryDataChannel stderr_output;
	madc::ProcessOptions options;
	options.args.push_back("pump");
	madc::Process process(madc::DataSource("exec://" + fixture_path()), options);
	madc::ProcessPumpResult result;
	madc::error err;

	REQUIRE(process.start(&err));
	REQUIRE(madc::pump_process(input, process, output, &stderr_output, result, &err));
	CHECK(result.input_bytes == input_bytes.size());
	CHECK(result.output_bytes == input_bytes.size());
	CHECK(result.stderr_bytes == input_bytes.size());
	CHECK(result.exit_status == 0);
	CHECK(output.bytes() == input_bytes);
	CHECK(stderr_output.bytes() == input_bytes);
}

TEST_CASE("format adapters stream typed records through process channels")
{
	madc::ProcessOptions options;
	options.args.push_back("echo");
	madc::Process process(madc::DataSource("exec://" + fixture_path()), options);
	madc::error err;
	LineFormat format;

	REQUIRE(process.start(&err));
	madc::ChannelOutputStream process_input(process.stdin_channel());
	madc::FormatSink<std::string> input(process_input, format);
	REQUIRE(input.put("first record", &err));
	REQUIRE(input.put("second record", &err));
	REQUIRE(input.close(&err));
	REQUIRE(process.close_stdin(&err));

	madc::ChannelInputStream process_output(process.stdout_channel());
	madc::FormatCursor<std::string> output(process_output, format);
	std::string record;
	REQUIRE(output.next(record));
	CHECK(record == "first record");
	REQUIRE(output.next(record));
	CHECK(record == "second record");
	CHECK_FALSE(output.next(record));
	CHECK(read_channel(process.stderr_channel(), &err).empty());
	REQUIRE(process.wait(&err));
	CHECK(process.exit_status() == 0);
}

TEST_CASE("completed processes release their owned descriptors")
{
	std::size_t before = open_fd_count();
	REQUIRE(before != 0);
	for ( int i = 0; i < 8; ++i )
	{
		madc::ProcessOptions options;
		options.args.push_back("argv");
		madc::Process process(madc::DataSource("exec://" + fixture_path()), options);
		madc::error err;
		REQUIRE(process.start(&err));
		REQUIRE(process.close_stdin(&err));
		CHECK(read_channel(process.stdout_channel(), &err).empty());
		CHECK(read_channel(process.stderr_channel(), &err).empty());
		REQUIRE(process.wait(&err));
	}
	CHECK(open_fd_count() == before);
}

TEST_CASE("process exec does not inherit unrelated data channels")
{
	const std::string path = "/tmp/madc_process_cloexec_"
		+ std::to_string(static_cast<long long>(getpid()));
	madc::error err;
	std::unique_ptr<madc::DataChannel> unrelated =
		madc::DataChannelRegistry::instance().open(
			madc::DataSource(std::string("file://") + path),
			madc::ChannelOpenMode::write, &err);
	REQUIRE(unrelated.get() != nullptr);

	madc::ProcessOptions options;
	options.args.push_back("fd-count");
	madc::Process process(madc::DataSource("exec://" + fixture_path()), options);
	REQUIRE(process.start(&err));
	REQUIRE(process.close_stdin(&err));
	std::vector<unsigned char> stdout_bytes =
		read_channel(process.stdout_channel(), &err);
	CHECK(read_channel(process.stderr_channel(), &err).empty());
	REQUIRE(process.wait(&err));
	CHECK(as_string(stdout_bytes) == "0\n");
	CHECK(process.exit_status() == 0);

	unrelated->close();
	std::remove(path.c_str());
}
