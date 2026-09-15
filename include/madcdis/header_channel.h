#ifndef __MADCDIS_HEADER_CHANNEL_H
#define __MADCDIS_HEADER_CHANNEL_H 1

#include "madcdis/datachannel.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace madc {
namespace detail {

// ---- the ONE HTTP-style header-block reader --------------------------------
// "Name: value" lines separated by CRLF, a blank line ending the block, the
// name matched case-insensitively. The WebSocket handshake (V6b-1) and the
// Content-Length message framing below are two readers of the SAME rule; a
// second private copy is how one of them would learn to accept a header the
// other rejects.

inline std::string header_trim(const std::string &s)
{
	std::size_t a = 0, b = s.size();
	while ( a < b && std::isspace((unsigned char)s[a]) ) ++a;
	while ( b > a && std::isspace((unsigned char)s[b - 1]) ) --b;
	return s.substr(a, b - a);
}

inline bool header_iequals(const std::string &a, const std::string &b)
{
	if ( a.size() != b.size() )
		return false;
	for ( std::size_t i = 0; i < a.size(); ++i )
		if ( std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]) )
			return false;
	return true;
}

// The value of header `name` in a header block, or false when absent.
inline bool header_value(const std::string &block, const char *name,
			 std::string &out)
{
	std::string want(name);
	std::size_t pos = 0;
	while ( pos < block.size() )
	{
		std::size_t eol = block.find("\r\n", pos);
		if ( eol == std::string::npos )
			eol = block.size();
		std::string line = block.substr(pos, eol - pos);
		std::size_t colon = line.find(':');
		if ( colon != std::string::npos
		  && header_iequals(header_trim(line.substr(0, colon)), want) )
		{
			out = header_trim(line.substr(colon + 1));
			return true;
		}
		pos = eol + 2;
	}
	return false;
}

} // namespace detail

// A Content-Length FRAMER over a byte channel (client-server V6c-2): the
// framing the Language Server Protocol, the Debug Adapter Protocol and the
// Build Server Protocol all use — a header block, a blank line, then exactly
// `Content-Length` bytes of body. Named for the FRAMING and not for LSP: the
// next protocol over it must find a framer, not write a second one.
//
// It OWNS a byte DataChannel and turns it into a MESSAGE channel, the shape
// WebSocketDataChannel established (V6b-1):
//   - as a DataChannel, read()/write() are RAW inner bytes (what the channel
//     object's park loop pumps into feed());
//   - as a PollableDataChannel, the poll handle IS the inner channel's, so a
//     serve task parks there exactly as it does on a plain socket;
//   - the message layer is feed() / next_message() / encode_message().
//
// Why the framing cannot live one layer up: a body is delimited by a BYTE
// COUNT, and a conforming peer may pretty-print JSON, so a line reader splits
// one message into several. The length is the only delimiter there is.
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): the channel's
// contract — single-threaded, confined to the task that owns it.
class HeaderFramedDataChannel : public DataChannel, public PollableDataChannel
{
public:
	HeaderFramedDataChannel(std::unique_ptr<DataChannel> inner,
				const std::string &leftover);
	~HeaderFramedDataChannel() override;

	// DataChannel — RAW inner I/O (the message layer is below).
	const char *name() const override;
	ChannelCapabilities capabilities() const override;
	bool read(void *buffer, std::size_t capacity, std::size_t &bytes_read,
		  error *err = nullptr) override;
	bool write(const void *buffer, std::size_t size,
		   std::size_t &bytes_written, error *err = nullptr) override;
	bool flush(error *err = nullptr) override;
	void close_read() override;
	void close_write() override;
	void cancel() override;
	void close() override;
	int exit_status() const override;
	bool is_terminal() const override;

	// PollableDataChannel — a serve task parks on the inner endpoint (and
	// its handle lives in the inner endpoint's space).
	intptr_t read_poll_handle() const override;
	poll_handle_kind read_poll_kind() const override;

	// Raw inner I/O for the channel object's message pump.
	bool read_raw(void *buffer, std::size_t capacity, std::size_t &bytes_read,
		      error *err = nullptr);
	bool write_raw(const void *buffer, std::size_t size, error *err = nullptr);

	// Message layer. feed() pushes raw inbound bytes into the decoder;
	// next_message() pulls the next complete message: true with `out` set
	// when one is ready, false when more bytes are needed OR the stream is
	// unusable. `eof` is true only in that second case — a header block
	// with no usable Content-Length cannot be resynced (the body length is
	// the only thing that says where the next message starts), so it ends
	// the stream with `why` carrying the reason. A ZERO-length body is a
	// message, not an end.
	void feed(const char *data, std::size_t n);
	bool next_message(std::string &out, bool &eof, std::string &why);

	// One framed message: the header block, the blank line, the body.
	std::string encode_message(const char *data, std::size_t n);

private:
	std::unique_ptr<DataChannel> inner_;
	std::string inbuf_;	// bytes not yet consumed by the decoder
	bool broken_;		// a refused header block: the stream is over
};

} // namespace madc

#endif // __MADCDIS_HEADER_CHANNEL_H
