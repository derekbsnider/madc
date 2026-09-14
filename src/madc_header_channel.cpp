// madc_header_channel.cpp — the Content-Length message framer (client-server
// V6c-2). A message channel over a byte channel: a header block, a blank line,
// then exactly `Content-Length` bytes. This is the Language Server Protocol's
// framing (and the Debug Adapter and Build Server protocols' — hence the name:
// the framing, not the protocol). See include/madcdis/header_channel.h for the
// layering and the thread-safety contract.

#include "madcdis/header_channel.h"

#include <cstdio>
#include <cstdlib>

namespace madc {

HeaderFramedDataChannel::HeaderFramedDataChannel(
		std::unique_ptr<DataChannel> inner, const std::string &leftover)
	: inner_(std::move(inner)), inbuf_(leftover), broken_(false)
{}

HeaderFramedDataChannel::~HeaderFramedDataChannel() {}

const char *HeaderFramedDataChannel::name() const { return "header-framed"; }

ChannelCapabilities HeaderFramedDataChannel::capabilities() const
{
	ChannelCapabilities c;
	c.read = true;
	c.write = true;
	c.half_close = true;
	return c;
}

bool HeaderFramedDataChannel::read(void *buffer, std::size_t capacity,
				   std::size_t &bytes_read, error *err)
{
	return read_raw(buffer, capacity, bytes_read, err);
}

bool HeaderFramedDataChannel::write(const void *buffer, std::size_t size,
				    std::size_t &bytes_written, error *err)
{
	if ( !write_raw(buffer, size, err) )
		return false;
	bytes_written = size;
	return true;
}

bool HeaderFramedDataChannel::flush(error *err) { return inner_->flush(err); }

void HeaderFramedDataChannel::close_read() { inner_->close_read(); }

void HeaderFramedDataChannel::close_write() { inner_->close_write(); }

void HeaderFramedDataChannel::close() { inner_->close(); }

intptr_t HeaderFramedDataChannel::read_poll_handle() const
{
	PollableDataChannel *p = pollable_surface(inner_.get());
	return p ? p->read_poll_handle() : -1;
}

bool HeaderFramedDataChannel::read_raw(void *buffer, std::size_t capacity,
				       std::size_t &bytes_read, error *err)
{
	return inner_->read(buffer, capacity, bytes_read, err);
}

bool HeaderFramedDataChannel::write_raw(const void *buffer, std::size_t size,
					error *err)
{
	return write_all(*inner_, buffer, size, err);
}

void HeaderFramedDataChannel::feed(const char *data, std::size_t n)
{
	inbuf_.append(data, n);
}

bool HeaderFramedDataChannel::next_message(std::string &out, bool &eof,
					   std::string &why)
{
	out.clear();
	eof = false;
	why.clear();
	if ( broken_ )
	{
		eof = true;
		return false;
	}
	std::size_t end = inbuf_.find("\r\n\r\n");
	if ( end == std::string::npos )
		return false;			// the block is still arriving

	const std::string block = inbuf_.substr(0, end + 2);	// keep the last CRLF
	std::string length_text;
	if ( !detail::header_value(block, "content-length", length_text) )
	{
		// Unresyncable: without the length nothing says where the next
		// message starts, so the stream ends here rather than silently
		// mis-framing everything after it.
		broken_ = true;
		eof = true;
		why = "no Content-Length header in the message block";
		return false;
	}
	char *stop = nullptr;
	const long long length = std::strtoll(length_text.c_str(), &stop, 10);
	if ( stop == length_text.c_str() || (stop && *stop != '\0') || length < 0 )
	{
		broken_ = true;
		eof = true;
		why = "Content-Length is not a length: `" + length_text + "`";
		return false;
	}

	const std::size_t body = end + 4;
	const std::size_t want = static_cast<std::size_t>(length);
	if ( inbuf_.size() - body < want )
		return false;			// the body is still arriving

	out.assign(inbuf_, body, want);
	inbuf_.erase(0, body + want);
	return true;
}

std::string HeaderFramedDataChannel::encode_message(const char *data,
						    std::size_t n)
{
	char header[64];
	std::snprintf(header, sizeof(header), "Content-Length: %llu\r\n\r\n",
		      (unsigned long long)n);
	std::string out(header);
	if ( n )
		out.append(data, n);
	return out;
}

} // namespace madc
