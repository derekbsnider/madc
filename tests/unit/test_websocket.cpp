#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "madcdis/websocket_channel.h"
#include "madcdis/datachannel.h"

#include <memory>
#include <string>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

using madc::WebSocketDataChannel;

namespace {

WebSocketDataChannel *make_ws(WebSocketDataChannel::Role role)
{
	std::unique_ptr<madc::DataChannel> inner(new madc::MemoryDataChannel());
	return new WebSocketDataChannel(std::move(inner), role, std::string());
}

// Build a client-masked frame by hand (opcode + FIN + payload) so the tests
// can exercise control and fragmented frames the public encoder does not emit.
std::string masked_frame(int opcode, bool fin, const std::string &payload)
{
	std::string f;
	f.push_back((char)((fin ? 0x80 : 0x00) | (opcode & 0x0f)));
	unsigned char len = (unsigned char)payload.size();	// tests stay < 126
	f.push_back((char)(0x80 | len));
	unsigned char mask[4] = { 0x11, 0x22, 0x33, 0x44 };
	for ( int i = 0; i < 4; ++i )
		f.push_back((char)mask[i]);
	for ( std::size_t i = 0; i < payload.size(); ++i )
		f.push_back((char)((unsigned char)payload[i] ^ mask[i & 3]));
	return f;
}

} // namespace

TEST_CASE("Sec-WebSocket-Accept matches the RFC6455 example vector")
{
	// RFC6455 §1.3: key "dGhlIHNhbXBsZSBub25jZQ==" -> accept
	// "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".
	CHECK(madc::websocket_accept_token("dGhlIHNhbXBsZSBub25jZQ==")
	      == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("server handshake yields a 101 with the accept token")
{
	std::string request =
		"GET /chat HTTP/1.1\r\n"
		"Host: example.com\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n";
	std::string response, reason;
	REQUIRE(madc::websocket_server_handshake(request, response, reason));
	CHECK(response.find("HTTP/1.1 101") == 0);
	CHECK(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n")
	      != std::string::npos);
	CHECK(response.find("\r\n\r\n") != std::string::npos);
}

TEST_CASE("a non-upgrade request is refused with a reason")
{
	std::string request = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
	std::string response, reason;
	CHECK_FALSE(madc::websocket_server_handshake(request, response, reason));
	CHECK_FALSE(reason.empty());
}

TEST_CASE("a text message round-trips client->server and server->client")
{
	std::unique_ptr<WebSocketDataChannel> client(
		make_ws(WebSocketDataChannel::Role::client));
	std::unique_ptr<WebSocketDataChannel> server(
		make_ws(WebSocketDataChannel::Role::server));

	// client -> server (client frames are masked; the server unmasks)
	std::string frame = client->encode_text("hello", 5);
	CHECK((unsigned char)frame[0] == 0x81);		// FIN + text
	CHECK(((unsigned char)frame[1] & 0x80) != 0);	// masked
	server->feed(frame.data(), frame.size());
	std::string out;
	bool eof = false;
	std::string reply;
	REQUIRE(server->next_message(out, eof, reply));
	CHECK(out == "hello");
	CHECK_FALSE(eof);
	CHECK(reply.empty());

	// server -> client (server frames are NOT masked)
	std::string sframe = server->encode_text("world!", 6);
	CHECK(((unsigned char)sframe[1] & 0x80) == 0);	// not masked
	client->feed(sframe.data(), sframe.size());
	std::string out2;
	bool eof2 = false;
	std::string reply2;
	REQUIRE(client->next_message(out2, eof2, reply2));
	CHECK(out2 == "world!");
}

TEST_CASE("a partial frame returns false until the rest arrives")
{
	std::unique_ptr<WebSocketDataChannel> client(
		make_ws(WebSocketDataChannel::Role::client));
	std::unique_ptr<WebSocketDataChannel> server(
		make_ws(WebSocketDataChannel::Role::server));

	std::string frame = client->encode_text("split", 5);
	server->feed(frame.data(), 3);			// only the first 3 bytes
	std::string out;
	bool eof = false;
	std::string reply;
	CHECK_FALSE(server->next_message(out, eof, reply));
	CHECK_FALSE(eof);
	server->feed(frame.data() + 3, frame.size() - 3);
	REQUIRE(server->next_message(out, eof, reply));
	CHECK(out == "split");
}

TEST_CASE("a ping is answered with a pong and yields no data message")
{
	std::unique_ptr<WebSocketDataChannel> server(
		make_ws(WebSocketDataChannel::Role::server));
	std::string ping = masked_frame(0x9, true, "pq");
	server->feed(ping.data(), ping.size());
	std::string out;
	bool eof = false;
	std::string reply;
	CHECK_FALSE(server->next_message(out, eof, reply));
	CHECK_FALSE(eof);
	REQUIRE(reply.size() >= 2);
	CHECK((unsigned char)reply[0] == 0x8A);		// FIN + pong
	CHECK((unsigned char)reply[1] == 0x02);		// unmasked, len 2
	CHECK(reply.substr(2) == "pq");
}

TEST_CASE("a close frame surfaces as EOF and echoes a close")
{
	std::unique_ptr<WebSocketDataChannel> server(
		make_ws(WebSocketDataChannel::Role::server));
	std::string close = masked_frame(0x8, true, "");
	server->feed(close.data(), close.size());
	std::string out;
	bool eof = false;
	std::string reply;
	CHECK_FALSE(server->next_message(out, eof, reply));
	CHECK(eof);
	REQUIRE(reply.size() >= 2);
	CHECK((unsigned char)reply[0] == 0x88);		// FIN + close
}

TEST_CASE("a fragmented message is reassembled across frames")
{
	std::unique_ptr<WebSocketDataChannel> server(
		make_ws(WebSocketDataChannel::Role::server));
	std::string f1 = masked_frame(0x1, false, "ab");	// text, not final
	std::string f2 = masked_frame(0x0, true, "cd");		// continuation, final
	server->feed(f1.data(), f1.size());
	std::string out;
	bool eof = false;
	std::string reply;
	CHECK_FALSE(server->next_message(out, eof, reply));	// first fragment only
	server->feed(f2.data(), f2.size());
	REQUIRE(server->next_message(out, eof, reply));
	CHECK(out == "abcd");
}
