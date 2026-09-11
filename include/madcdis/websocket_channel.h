#ifndef __MADCDIS_WEBSOCKET_CHANNEL_H
#define __MADCDIS_WEBSOCKET_CHANNEL_H 1

#include "madcdis/datachannel.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace madc {

// An RFC6455 WebSocket framer over a byte channel (client-server V6b, no
// extensions / no compression). It OWNS an accepted (or connected) byte
// DataChannel and turns it into a MESSAGE channel: each send is one text
// frame, each receive one complete message. Control frames (ping/pong/close)
// are handled internally.
//
// The HANDSHAKE is driven one layer up (madc::channel::upgrade_websocket /
// connect_websocket), where the cooperative-scheduler park hook lives, so a
// serve task parks on the socket instead of blocking the OS thread. This class
// is the pure codec plus the raw inner I/O the pump needs:
//   - as a DataChannel, read()/write() are RAW inner-socket bytes (what the
//     channel object's park loop pumps into feed());
//   - as a PollableDataChannel, the poll handle IS the inner socket's, so a
//     serve task parks there exactly as it does on a plain socket;
//   - the message layer is feed() / next_message() / encode_text().
class WebSocketDataChannel : public DataChannel, public PollableDataChannel
{
public:
	enum class Role { server, client };

	WebSocketDataChannel(std::unique_ptr<DataChannel> inner, Role role,
			     const std::string &leftover);
	~WebSocketDataChannel() override;

	// DataChannel — RAW inner-socket I/O (the message layer is below).
	const char *name() const override;
	ChannelCapabilities capabilities() const override;
	bool read(void *buffer, std::size_t capacity, std::size_t &bytes_read,
		  error *err = nullptr) override;
	bool write(const void *buffer, std::size_t size,
		   std::size_t &bytes_written, error *err = nullptr) override;
	bool flush(error *err = nullptr) override;
	void close_read() override;
	void close_write() override;
	void close() override;

	// PollableDataChannel — a serve task parks on the inner socket fd.
	intptr_t read_poll_handle() const override;

	// Raw inner I/O for the channel object's message pump.
	bool read_raw(void *buffer, std::size_t capacity, std::size_t &bytes_read,
		      error *err = nullptr);
	bool write_raw(const void *buffer, std::size_t size, error *err = nullptr);

	// Message layer. feed() pushes raw inbound bytes into the decoder;
	// next_message() pulls the next complete DATA message: true with `out`
	// set when one is ready, false when more bytes are needed OR the peer
	// closed (`eof` true only in the close case). Any control response the
	// scan produced (a pong, a close echo) is appended to `reply` for the
	// caller to write back down the socket.
	void feed(const char *data, std::size_t n);
	bool next_message(std::string &out, bool &eof, std::string &reply);

	// Encode one FIN text frame (masked in the client role, per RFC6455).
	std::string encode_text(const char *data, std::size_t n);

private:
	std::string encode_frame(int opcode, const char *data, std::size_t n);
	void next_mask(unsigned char out[4]);

	std::unique_ptr<DataChannel> inner_;
	Role role_;
	std::string inbuf_;	// undecoded inbound bytes
	std::string fragment_;	// assembled payload of an in-progress message
	int frag_opcode_;	// opcode of the fragmented message (0 = none)
	bool close_sent_;	// a close echo was already queued
	uint32_t mask_state_;	// masking-nonce PRNG (client role)
};

// The server-side upgrade: parse `request` (an HTTP/1.1 GET carrying the
// WebSocket headers) and, when valid, compose the 101 response into `response`
// (true); false + `reason` on a request that is not a WebSocket upgrade.
bool websocket_server_handshake(const std::string &request,
				std::string &response, std::string &reason);
// Compose a client upgrade request for `resource` (the GET target) to `host`.
// `key_out` receives the generated Sec-WebSocket-Key so the caller can
// validate the server's 101 accept token.
std::string websocket_client_request(const std::string &resource,
				     const std::string &host, std::string &key_out);
// Validate a server's 101 `response` against the key we sent (true = accepted;
// false + `reason` otherwise).
bool websocket_client_validate(const std::string &response,
			       const std::string &key, std::string &reason);
// base64(SHA1(key + GUID)) — the Sec-WebSocket-Accept token (exposed so a unit
// test can pin it to the RFC6455 §1.3 example vector).
std::string websocket_accept_token(const std::string &key);

} // namespace madc

#endif // __MADCDIS_WEBSOCKET_CHANNEL_H
