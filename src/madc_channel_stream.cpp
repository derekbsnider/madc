#include "madcdis/channel_stream.h"

namespace madc {

ChannelInputStreamBuffer::ChannelInputStreamBuffer(DataChannel &channel)
	: channel_(channel), failed_(false)
{
	setg(buffer_, buffer_, buffer_);
}

bool ChannelInputStreamBuffer::failed() const { return failed_; }
const error &ChannelInputStreamBuffer::last_error() const { return error_; }

ChannelInputStreamBuffer::int_type ChannelInputStreamBuffer::underflow()
{
	if ( gptr() < egptr() )
		return traits_type::to_int_type(*gptr());
	std::size_t count = 0;
	if ( !channel_.read(buffer_, sizeof(buffer_), count, &error_) )
	{
		failed_ = true;
		return traits_type::eof();
	}
	if ( count == 0 )
		return traits_type::eof();
	setg(buffer_, buffer_, buffer_ + count);
	return traits_type::to_int_type(*gptr());
}

ChannelOutputStreamBuffer::ChannelOutputStreamBuffer(DataChannel &channel)
	: channel_(channel), failed_(false)
{}

bool ChannelOutputStreamBuffer::failed() const { return failed_; }
const error &ChannelOutputStreamBuffer::last_error() const { return error_; }

ChannelOutputStreamBuffer::int_type
ChannelOutputStreamBuffer::overflow(int_type ch)
{
	if ( traits_type::eq_int_type(ch, traits_type::eof()) )
		return sync() == 0 ? traits_type::not_eof(ch) : traits_type::eof();
	char value = traits_type::to_char_type(ch);
	return xsputn(&value, 1) == 1 ? ch : traits_type::eof();
}

std::streamsize ChannelOutputStreamBuffer::xsputn(const char *data,
						  std::streamsize size)
{
	if ( size <= 0 )
		return 0;
	if ( !write_all(channel_, data, static_cast<std::size_t>(size), &error_) )
	{
		failed_ = true;
		return 0;
	}
	return size;
}

int ChannelOutputStreamBuffer::sync()
{
	if ( channel_.flush(&error_) )
		return 0;
	failed_ = true;
	return -1;
}

ChannelInputStream::ChannelInputStream(DataChannel &channel)
	: std::istream(nullptr), buffer_(channel)
{
	rdbuf(&buffer_);
}

bool ChannelInputStream::channel_failed() const { return buffer_.failed(); }
const error &ChannelInputStream::channel_error() const { return buffer_.last_error(); }

ChannelOutputStream::ChannelOutputStream(DataChannel &channel)
	: std::ostream(nullptr), buffer_(channel)
{
	rdbuf(&buffer_);
}

bool ChannelOutputStream::channel_failed() const { return buffer_.failed(); }
const error &ChannelOutputStream::channel_error() const { return buffer_.last_error(); }

} // namespace madc
