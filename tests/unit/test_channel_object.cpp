#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "madcdis/channel.h"

#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
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

TEST_CASE("channel listens, accepts a client, and round-trips as a listener facet")
{
	madc::channel listener("listen://127.0.0.1:0");
	REQUIRE(listener.ok());

	// The listener reports the ephemeral port getsockname assigned.
	std::string bound = listener.local_endpoint();
	std::size_t colon = bound.rfind(':');
	REQUIRE(colon != std::string::npos);
	uint16_t port = static_cast<uint16_t>(std::stoi(bound.substr(colon + 1)));
	REQUIRE(port != 0);

	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in address;
	std::memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&address),
			  sizeof(address)) == 0);

	// accept() takes the pending connection into an empty channel.
	madc::channel client;
	int64_t accepted = 0;
	for ( int attempt = 0; attempt < 100 && accepted == 0; ++attempt )
	{
		accepted = listener.accept(client);
		if ( accepted != 0 )
			break;
		int handle = static_cast<int>(listener.read_wait_handle());
		if ( handle < 0 )
			break;
		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(handle, &readable);
		timeval timeout;
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;
		::select(handle + 1, &readable, nullptr, nullptr, &timeout);
	}
	REQUIRE(accepted == 1);
	REQUIRE(client.ok());

	// raw client -> accepted channel (line semantics on the accepted side).
	const char request[] = "listen-object-request\n";
	REQUIRE(::send(fd, request, sizeof(request) - 1, 0)
		== static_cast<ssize_t>(sizeof(request) - 1));
	std::string line;
	REQUIRE(client.readline(line));
	CHECK(line == "listen-object-request");

	// accepted channel -> raw client.
	REQUIRE(client.write("object-response\n"));
	char buffer[64] = {};
	ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
	REQUIRE(got == static_cast<ssize_t>(std::strlen("object-response\n")));
	CHECK(std::string(buffer, got) == "object-response\n");

	::close(fd);
	client.close();
	listener.close();

	// accept() on a non-listener (a closed one) refuses with -1.
	madc::channel plain;
	CHECK(listener.accept(plain) == -1);
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
