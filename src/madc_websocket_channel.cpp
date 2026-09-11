// madc_websocket_channel.cpp — the RFC6455 WebSocket framer (client-server
// V6b). A message channel over a byte channel: text frames out, complete
// messages in, ping/pong/close handled internally. No extensions. The
// handshake token needs SHA-1 + base64 — neither is available engine-side
// (js::btoa is the script surface, cross-layer), so both live here, small and
// self-contained. See include/madcdis/websocket_channel.h for the layering.

#include "madcdis/websocket_channel.h"

#include <cctype>
#include <ctime>

namespace madc {
namespace {

// ---- SHA-1 (FIPS 180-1), one-shot over a byte buffer -----------------------

inline uint32_t rol32(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

void sha1(const unsigned char *msg, std::size_t len, unsigned char out[20])
{
	uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
		 h3 = 0x10325476, h4 = 0xC3D2E1F0;
	// Pad: message || 0x80 || 0x00... || 64-bit big-endian bit length.
	std::string data(reinterpret_cast<const char *>(msg), len);
	uint64_t bitlen = static_cast<uint64_t>(len) * 8;
	data.push_back((char)0x80);
	while ( data.size() % 64 != 56 )
		data.push_back((char)0x00);
	for ( int i = 7; i >= 0; --i )
		data.push_back((char)((bitlen >> (8 * i)) & 0xff));

	const unsigned char *p = reinterpret_cast<const unsigned char *>(data.data());
	for ( std::size_t off = 0; off < data.size(); off += 64 )
	{
		uint32_t w[80];
		for ( int i = 0; i < 16; ++i )
			w[i] = (p[off + i * 4] << 24) | (p[off + i * 4 + 1] << 16)
			     | (p[off + i * 4 + 2] << 8) | p[off + i * 4 + 3];
		for ( int i = 16; i < 80; ++i )
			w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

		uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
		for ( int i = 0; i < 80; ++i )
		{
			uint32_t f, k;
			if ( i < 20 )      { f = (b & c) | ((~b) & d);       k = 0x5A827999; }
			else if ( i < 40 ) { f = b ^ c ^ d;                 k = 0x6ED9EBA1; }
			else if ( i < 60 ) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
			else               { f = b ^ c ^ d;                 k = 0xCA62C1D6; }
			uint32_t t = rol32(a, 5) + f + e + k + w[i];
			e = d; d = c; c = rol32(b, 30); b = a; a = t;
		}
		h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
	}
	uint32_t h[5] = { h0, h1, h2, h3, h4 };
	for ( int i = 0; i < 5; ++i )
	{
		out[i * 4]     = (unsigned char)((h[i] >> 24) & 0xff);
		out[i * 4 + 1] = (unsigned char)((h[i] >> 16) & 0xff);
		out[i * 4 + 2] = (unsigned char)((h[i] >> 8) & 0xff);
		out[i * 4 + 3] = (unsigned char)(h[i] & 0xff);
	}
}

// ---- base64 encode ---------------------------------------------------------

std::string base64_encode(const unsigned char *in, std::size_t len)
{
	static const char T[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	for ( std::size_t i = 0; i < len; i += 3 )
	{
		std::size_t remain = len - i;
		unsigned char b0 = in[i];
		unsigned char b1 = remain > 1 ? in[i + 1] : 0;
		unsigned char b2 = remain > 2 ? in[i + 2] : 0;
		out += T[(b0 >> 2) & 0x3F];
		out += T[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)];
		out += remain > 1 ? T[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)] : '=';
		out += remain > 2 ? T[b2 & 0x3F] : '=';
	}
	return out;
}

// ---- header parsing helpers ------------------------------------------------

std::string trim(const std::string &s)
{
	std::size_t a = 0, b = s.size();
	while ( a < b && std::isspace((unsigned char)s[a]) ) ++a;
	while ( b > a && std::isspace((unsigned char)s[b - 1]) ) --b;
	return s.substr(a, b - a);
}

bool iequals(const std::string &a, const std::string &b)
{
	if ( a.size() != b.size() )
		return false;
	for ( std::size_t i = 0; i < a.size(); ++i )
		if ( std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]) )
			return false;
	return true;
}

// The value of header `name` (case-insensitive) in an HTTP header block, or
// false when absent. Header lines are "Name: value" separated by CRLF.
bool header_value(const std::string &block, const char *name, std::string &out)
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
		if ( colon != std::string::npos && iequals(trim(line.substr(0, colon)), want) )
		{
			out = trim(line.substr(colon + 1));
			return true;
		}
		pos = eol + 2;
	}
	return false;
}

std::string first_line(const std::string &block)
{
	std::size_t eol = block.find("\r\n");
	return eol == std::string::npos ? block : block.substr(0, eol);
}

} // namespace

// ---- handshake API ---------------------------------------------------------

std::string websocket_accept_token(const std::string &key)
{
	static const char *GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	std::string s = key + GUID;
	unsigned char digest[20];
	sha1(reinterpret_cast<const unsigned char *>(s.data()), s.size(), digest);
	return base64_encode(digest, 20);
}

bool websocket_server_handshake(const std::string &request,
				std::string &response, std::string &reason)
{
	std::string upgrade;
	if ( !header_value(request, "upgrade", upgrade)
	  || !iequals(trim(upgrade), "websocket") )
	{
		reason = "not a WebSocket upgrade (missing Upgrade: websocket)";
		return false;
	}
	std::string key;
	if ( !header_value(request, "sec-websocket-key", key) || trim(key).empty() )
	{
		reason = "missing Sec-WebSocket-Key";
		return false;
	}
	response = "HTTP/1.1 101 Switching Protocols\r\n"
		   "Upgrade: websocket\r\n"
		   "Connection: Upgrade\r\n"
		   "Sec-WebSocket-Accept: " + websocket_accept_token(trim(key))
		 + "\r\n\r\n";
	return true;
}

std::string websocket_client_request(const std::string &resource,
				     const std::string &host, std::string &key_out)
{
	// A 16-byte nonce → base64. Not security-critical (a handshake nonce);
	// a time+counter seed is sufficient to make the accept token unique.
	static uint32_t counter = 0;
	unsigned char nonce[16];
	uint32_t seed = (uint32_t)std::time(nullptr) ^ (counter += 0x9E3779B9u);
	for ( int i = 0; i < 16; ++i )
	{
		seed = seed * 1664525u + 1013904223u;
		nonce[i] = (unsigned char)(seed >> 24);
	}
	key_out = base64_encode(nonce, 16);
	std::string r = resource.empty() ? std::string("/") : resource;
	std::string h = host.empty() ? std::string("localhost") : host;
	return "GET " + r + " HTTP/1.1\r\n"
	       "Host: " + h + "\r\n"
	       "Upgrade: websocket\r\n"
	       "Connection: Upgrade\r\n"
	       "Sec-WebSocket-Key: " + key_out + "\r\n"
	       "Sec-WebSocket-Version: 13\r\n\r\n";
}

bool websocket_client_validate(const std::string &response,
			       const std::string &key, std::string &reason)
{
	if ( first_line(response).find(" 101") == std::string::npos )
	{
		reason = "server did not switch protocols (no 101)";
		return false;
	}
	std::string accept;
	if ( !header_value(response, "sec-websocket-accept", accept) )
	{
		reason = "missing Sec-WebSocket-Accept";
		return false;
	}
	if ( trim(accept) != websocket_accept_token(key) )
	{
		reason = "Sec-WebSocket-Accept token mismatch";
		return false;
	}
	return true;
}

// ---- the framer ------------------------------------------------------------

WebSocketDataChannel::WebSocketDataChannel(std::unique_ptr<DataChannel> inner,
					   Role role, const std::string &leftover)
	: inner_(std::move(inner)), role_(role), frag_opcode_(0),
	  close_sent_(false),
	  mask_state_((uint32_t)std::time(nullptr) ^ 0xC5AB0DC8u)
{
	// Any bytes read past the handshake boundary belong to the frame stream.
	if ( !leftover.empty() )
		inbuf_.append(leftover);
}

WebSocketDataChannel::~WebSocketDataChannel() {}

const char *WebSocketDataChannel::name() const { return "ws"; }

ChannelCapabilities WebSocketDataChannel::capabilities() const
{
	ChannelCapabilities c;
	c.read = true;
	c.write = true;
	c.half_close = true;
	return c;
}

bool WebSocketDataChannel::read(void *buffer, std::size_t capacity,
				std::size_t &bytes_read, error *err)
{
	return read_raw(buffer, capacity, bytes_read, err);
}

bool WebSocketDataChannel::write(const void *buffer, std::size_t size,
				 std::size_t &bytes_written, error *err)
{
	if ( !write_raw(buffer, size, err) )
		return false;
	bytes_written = size;
	return true;
}

bool WebSocketDataChannel::flush(error *err) { return inner_->flush(err); }

void WebSocketDataChannel::close_read() { inner_->close_read(); }

void WebSocketDataChannel::close_write()
{
	// A courteous close: send a close frame (best effort) before the FIN.
	if ( !close_sent_ )
	{
		std::string f = encode_frame(0x8, nullptr, 0);
		write_raw(f.data(), f.size(), nullptr);
		close_sent_ = true;
	}
	inner_->close_write();
}

void WebSocketDataChannel::close() { inner_->close(); }

intptr_t WebSocketDataChannel::read_poll_handle() const
{
	PollableDataChannel *p = pollable_surface(inner_.get());
	return p ? p->read_poll_handle() : -1;
}

bool WebSocketDataChannel::read_raw(void *buffer, std::size_t capacity,
				    std::size_t &bytes_read, error *err)
{
	return inner_->read(buffer, capacity, bytes_read, err);
}

bool WebSocketDataChannel::write_raw(const void *buffer, std::size_t size,
				     error *err)
{
	return write_all(*inner_, buffer, size, err);
}

void WebSocketDataChannel::feed(const char *data, std::size_t n)
{
	inbuf_.append(data, n);
}

bool WebSocketDataChannel::next_message(std::string &out, bool &eof,
					std::string &reply)
{
	eof = false;
	for ( ;; )
	{
		if ( inbuf_.size() < 2 )
			return false;
		const unsigned char *p =
			reinterpret_cast<const unsigned char *>(inbuf_.data());
		std::size_t avail = inbuf_.size();
		bool fin = (p[0] & 0x80) != 0;
		int opcode = p[0] & 0x0f;
		bool masked = (p[1] & 0x80) != 0;
		uint64_t len = p[1] & 0x7f;
		std::size_t pos = 2;
		if ( len == 126 )
		{
			if ( avail < pos + 2 )
				return false;
			len = ((uint64_t)p[pos] << 8) | p[pos + 1];
			pos += 2;
		}
		else if ( len == 127 )
		{
			if ( avail < pos + 8 )
				return false;
			len = 0;
			for ( int i = 0; i < 8; ++i )
				len = (len << 8) | p[pos + i];
			pos += 8;
		}
		unsigned char mask[4] = { 0, 0, 0, 0 };
		if ( masked )
		{
			if ( avail < pos + 4 )
				return false;
			for ( int i = 0; i < 4; ++i )
				mask[i] = p[pos + i];
			pos += 4;
		}
		if ( avail < pos + (std::size_t)len )
			return false;	// the full payload is not buffered yet

		std::string payload;
		payload.resize((std::size_t)len);
		for ( uint64_t i = 0; i < len; ++i )
			payload[(std::size_t)i] = masked
				? (char)(p[pos + i] ^ mask[i & 3])
				: (char)p[pos + i];
		inbuf_.erase(0, pos + (std::size_t)len);	// consume the frame

		if ( opcode == 0x8 )		// close
		{
			if ( !close_sent_ )
			{
				reply += encode_frame(0x8, nullptr, 0);
				close_sent_ = true;
			}
			eof = true;
			return false;
		}
		if ( opcode == 0x9 )		// ping -> pong (echo payload)
		{
			reply += encode_frame(0xA, payload.data(), payload.size());
			continue;
		}
		if ( opcode == 0xA )		// pong -> ignore
			continue;
		if ( opcode == 0x0 )		// continuation of a fragmented message
		{
			fragment_ += payload;
			if ( fin )
			{
				out.swap(fragment_);
				fragment_.clear();
				frag_opcode_ = 0;
				return true;
			}
			continue;
		}
		// 0x1 text / 0x2 binary — a data message.
		if ( fin )
		{
			out.swap(payload);
			return true;
		}
		fragment_ = payload;		// first fragment
		frag_opcode_ = opcode;
	}
}

void WebSocketDataChannel::next_mask(unsigned char out[4])
{
	for ( int i = 0; i < 4; ++i )
	{
		mask_state_ = mask_state_ * 1664525u + 1013904223u;
		out[i] = (unsigned char)(mask_state_ >> 24);
	}
}

std::string WebSocketDataChannel::encode_frame(int opcode, const char *data,
					       std::size_t n)
{
	std::string f;
	f.push_back((char)(0x80 | (opcode & 0x0f)));	// FIN + opcode
	bool mask = (role_ == Role::client);		// clients MUST mask
	unsigned char b1 = mask ? 0x80 : 0x00;
	if ( n < 126 )
		f.push_back((char)(b1 | (unsigned char)n));
	else if ( n < 65536 )
	{
		f.push_back((char)(b1 | 126));
		f.push_back((char)((n >> 8) & 0xff));
		f.push_back((char)(n & 0xff));
	}
	else
	{
		f.push_back((char)(b1 | 127));
		for ( int i = 7; i >= 0; --i )
			f.push_back((char)(((uint64_t)n >> (8 * i)) & 0xff));
	}
	if ( mask )
	{
		unsigned char mk[4];
		next_mask(mk);
		for ( int i = 0; i < 4; ++i )
			f.push_back((char)mk[i]);
		for ( std::size_t i = 0; i < n; ++i )
			f.push_back((char)((unsigned char)data[i] ^ mk[i & 3]));
	}
	else if ( n )
		f.append(data, n);
	return f;
}

std::string WebSocketDataChannel::encode_text(const char *data, std::size_t n)
{
	return encode_frame(0x1, data, n);
}

} // namespace madc
