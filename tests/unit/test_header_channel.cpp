#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "madcdis/header_channel.h"
#include "madcdis/datachannel.h"

#include <memory>
#include <string>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

using madc::HeaderFramedDataChannel;

namespace {

HeaderFramedDataChannel *make_hdr()
{
	std::unique_ptr<madc::DataChannel> inner(new madc::MemoryDataChannel());
	return new HeaderFramedDataChannel(std::move(inner), std::string());
}

// One framed message, the way a conforming peer writes it.
std::string framed(const std::string &body)
{
	char header[64];
	std::snprintf(header, sizeof(header), "Content-Length: %u\r\n\r\n",
		      (unsigned)body.size());
	return std::string(header) + body;
}

} // namespace

TEST_CASE("one message arrives whole however the bytes are split")
{
	std::unique_ptr<HeaderFramedDataChannel> h(make_hdr());
	const std::string wire = framed("{\"id\":1}");
	std::string out, why;
	bool eof = false;

	// Feed it one byte at a time: no message until the last byte lands.
	for ( std::size_t i = 0; i + 1 < wire.size(); ++i )
	{
		h->feed(wire.data() + i, 1);
		CHECK(h->next_message(out, eof, why) == false);
		CHECK(eof == false);
		CHECK(why.empty());
	}
	h->feed(wire.data() + wire.size() - 1, 1);
	REQUIRE(h->next_message(out, eof, why) == true);
	CHECK(out == "{\"id\":1}");
	CHECK(why.empty());
}

TEST_CASE("two messages in one feed come back in order")
{
	std::unique_ptr<HeaderFramedDataChannel> h(make_hdr());
	const std::string wire = framed("first") + framed("second");
	h->feed(wire.data(), wire.size());

	std::string out, why;
	bool eof = false;
	REQUIRE(h->next_message(out, eof, why) == true);
	CHECK(out == "first");
	REQUIRE(h->next_message(out, eof, why) == true);
	CHECK(out == "second");
	CHECK(h->next_message(out, eof, why) == false);	// nothing left
	CHECK(eof == false);
}

TEST_CASE("a body carrying newlines is delivered whole")
{
	// A pretty-printing peer is conforming: the LENGTH delimits a message,
	// never a line ending. This is the case a readline-based reader gets
	// wrong, and the reason the framing lives here.
	std::unique_ptr<HeaderFramedDataChannel> h(make_hdr());
	const std::string body = "{\n  \"jsonrpc\": \"2.0\",\n  \"id\": 7\n}";
	const std::string wire = framed(body);
	h->feed(wire.data(), wire.size());

	std::string out, why;
	bool eof = false;
	REQUIRE(h->next_message(out, eof, why) == true);
	CHECK(out == body);
}

TEST_CASE("a zero-length body is a message, not an EOF")
{
	std::unique_ptr<HeaderFramedDataChannel> h(make_hdr());
	const std::string wire = framed("");
	h->feed(wire.data(), wire.size());

	std::string out, why;
	bool eof = false;
	REQUIRE(h->next_message(out, eof, why) == true);
	CHECK(out.empty());
	CHECK(eof == false);
}

TEST_CASE("other headers are tolerated, in any order and any case")
{
	std::unique_ptr<HeaderFramedDataChannel> h(make_hdr());
	const std::string body = "{}";
	const std::string wire =
		"Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n"
		"content-length: 2\r\n"
		"\r\n" + body;
	h->feed(wire.data(), wire.size());

	std::string out, why;
	bool eof = false;
	REQUIRE(h->next_message(out, eof, why) == true);
	CHECK(out == body);
}

TEST_CASE("a header block with no Content-Length is refused, not awaited")
{
	std::unique_ptr<HeaderFramedDataChannel> h(make_hdr());
	const std::string wire = "Content-Type: text/plain\r\n\r\n{}";
	h->feed(wire.data(), wire.size());

	std::string out, why;
	bool eof = false;
	CHECK(h->next_message(out, eof, why) == false);
	CHECK(eof == true);			// the stream cannot be resynced
	CHECK(why.find("Content-Length") != std::string::npos);
}

TEST_CASE("a non-numeric Content-Length is refused")
{
	std::unique_ptr<HeaderFramedDataChannel> h(make_hdr());
	const std::string wire = "Content-Length: twelve\r\n\r\n{}";
	h->feed(wire.data(), wire.size());

	std::string out, why;
	bool eof = false;
	CHECK(h->next_message(out, eof, why) == false);
	CHECK(eof == true);
	CHECK(why.find("Content-Length") != std::string::npos);
}

TEST_CASE("encode_message writes the header the spec names")
{
	std::unique_ptr<HeaderFramedDataChannel> h(make_hdr());
	CHECK(h->encode_message("{}", 2) == "Content-Length: 2\r\n\r\n{}");
	CHECK(h->encode_message("", 0) == "Content-Length: 0\r\n\r\n");
}

TEST_CASE("what encode writes, feed reads back")
{
	std::unique_ptr<HeaderFramedDataChannel> h(make_hdr());
	const std::string body = "{\"method\":\"initialize\",\"id\":1}";
	const std::string wire = h->encode_message(body.data(), body.size());
	h->feed(wire.data(), wire.size());

	std::string out, why;
	bool eof = false;
	REQUIRE(h->next_message(out, eof, why) == true);
	CHECK(out == body);
}

TEST_CASE("leftover bytes handed in at construction are the first message")
{
	// The facet switch hands over whatever the byte reader already
	// buffered — those bytes are the head of the message stream.
	std::unique_ptr<madc::DataChannel> inner(new madc::MemoryDataChannel());
	const std::string wire = framed("early");
	HeaderFramedDataChannel h(std::move(inner), wire);

	std::string out, why;
	bool eof = false;
	REQUIRE(h.next_message(out, eof, why) == true);
	CHECK(out == "early");
}
