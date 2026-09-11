#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "../src/madc_io_reactor.h"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

namespace {

// The op_kind as an int — doctest stringifies scoped enums unevenly across
// versions, so the CHECKs compare the underlying value.
int kind_of(madc::io::op_kind k)
{
	return static_cast<int>(k);
}

// Wait (up to ~2s) for the next single completion, tolerating spurious
// zero-returns from wait() (a doorbell wake a prior drain already emptied).
bool next_completion(madc::io::Reactor &r, madc::io::completion &out)
{
	for ( int i = 0; i < 200; ++i )
	{
		madc::io::completion c[1];
		if ( r.wait(c, 1, 10) == 1 )
		{
			out = c[0];
			return true;
		}
	}
	return false;
}

int loopback_listener(uint16_t &port)
{
	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if ( fd < 0 )
		return -1;
	sockaddr_in address;
	std::memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if ( ::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
	  || ::listen(fd, 4) != 0 )
	{
		::close(fd);
		return -1;
	}
	socklen_t len = sizeof(address);
	::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &len);
	port = ntohs(address.sin_port);
	return fd;
}

int dial(uint16_t port)
{
	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if ( fd < 0 )
		return -1;
	sockaddr_in address;
	std::memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if ( ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 )
	{
		::close(fd);
		return -1;
	}
	return fd;
}

} // namespace

TEST_CASE("io reactor: accept, read, write, close completions over loopback")
{
	if ( !madc::io::Reactor::available() )
		return;		// no backend on this platform yet (mac/win)

	madc::io::Reactor reactor;
	int tag_accept, tag_read, tag_write, tag_close;

	uint16_t port = 0;
	int listener = loopback_listener(port);
	REQUIRE(listener >= 0);

	// Submit an accept, then a client dials the bound port.
	uint64_t accept_id = reactor.submit_accept(listener, &tag_accept);
	int client = dial(port);
	REQUIRE(client >= 0);

	madc::io::completion c;
	REQUIRE(next_completion(reactor, c));
	CHECK(kind_of(c.kind) == kind_of(madc::io::op_kind::accept));
	CHECK(c.id == accept_id);
	CHECK(c.user == &tag_accept);
	REQUIRE(c.result >= 0);		// the accepted server-side fd
	int server = c.result;

	// client -> server: a submitted read completes with the sent bytes.
	char buffer[64] = {};
	uint64_t read_id = reactor.submit_read(server, buffer, sizeof(buffer),
					       &tag_read);
	const char message[] = "reactor-hello";
	REQUIRE(::send(client, message, sizeof(message) - 1, 0)
		== static_cast<ssize_t>(sizeof(message) - 1));
	REQUIRE(next_completion(reactor, c));
	CHECK(kind_of(c.kind) == kind_of(madc::io::op_kind::read));
	CHECK(c.id == read_id);
	CHECK(c.user == &tag_read);
	REQUIRE(c.result == static_cast<int>(sizeof(message) - 1));
	CHECK(std::string(buffer, c.result) == "reactor-hello");

	// server -> client: a submitted write completes and the client receives.
	const char reply[] = "reactor-reply";
	uint64_t write_id = reactor.submit_write(server, reply, sizeof(reply) - 1,
						 &tag_write);
	REQUIRE(next_completion(reactor, c));
	CHECK(kind_of(c.kind) == kind_of(madc::io::op_kind::write));
	CHECK(c.id == write_id);
	REQUIRE(c.result == static_cast<int>(sizeof(reply) - 1));
	char received[64] = {};
	REQUIRE(::recv(client, received, sizeof(received), 0)
		== static_cast<ssize_t>(sizeof(reply) - 1));
	CHECK(std::string(received, sizeof(reply) - 1) == "reactor-reply");

	// close the server fd through the reactor.
	uint64_t close_id = reactor.submit_close(server, &tag_close);
	REQUIRE(next_completion(reactor, c));
	CHECK(kind_of(c.kind) == kind_of(madc::io::op_kind::close));
	CHECK(c.id == close_id);
	CHECK(c.result == 0);

	::close(client);
	::close(listener);
	// reactor's destructor stops + joins the I/O thread.
}

TEST_CASE("io reactor: a read completion reports EOF as zero")
{
	if ( !madc::io::Reactor::available() )
		return;

	madc::io::Reactor reactor;
	uint16_t port = 0;
	int listener = loopback_listener(port);
	REQUIRE(listener >= 0);
	reactor.submit_accept(listener, nullptr);
	int client = dial(port);
	REQUIRE(client >= 0);

	madc::io::completion c;
	REQUIRE(next_completion(reactor, c));
	REQUIRE(kind_of(c.kind) == kind_of(madc::io::op_kind::accept));
	int server = c.result;
	REQUIRE(server >= 0);

	// The client hangs up; a read on the server side completes with 0.
	::close(client);
	char buffer[16] = {};
	reactor.submit_read(server, buffer, sizeof(buffer), nullptr);
	REQUIRE(next_completion(reactor, c));
	CHECK(kind_of(c.kind) == kind_of(madc::io::op_kind::read));
	CHECK(c.result == 0);		// EOF

	::close(server);
	::close(listener);
}
