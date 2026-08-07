#ifndef __MADCDIS_CHANNEL_STREAM_H
#define __MADCDIS_CHANNEL_STREAM_H 1

#include "madcdis/datachannel.h"

#include <istream>
#include <ostream>
#include <streambuf>

namespace madc {

class ChannelInputStreamBuffer : public std::streambuf
{
public:
	explicit ChannelInputStreamBuffer(DataChannel &channel);

	bool failed() const;
	const error &last_error() const;

protected:
	int_type underflow() override;

private:
	DataChannel &channel_;
	char buffer_[4096];
	bool failed_;
	error error_;
};

class ChannelOutputStreamBuffer : public std::streambuf
{
public:
	explicit ChannelOutputStreamBuffer(DataChannel &channel);

	bool failed() const;
	const error &last_error() const;

protected:
	int_type overflow(int_type ch) override;
	std::streamsize xsputn(const char *data, std::streamsize size) override;
	int sync() override;

private:
	DataChannel &channel_;
	bool failed_;
	error error_;
};

class ChannelInputStream : public std::istream
{
public:
	explicit ChannelInputStream(DataChannel &channel);

	bool channel_failed() const;
	const error &channel_error() const;

private:
	ChannelInputStreamBuffer buffer_;
};

class ChannelOutputStream : public std::ostream
{
public:
	explicit ChannelOutputStream(DataChannel &channel);

	bool channel_failed() const;
	const error &channel_error() const;

private:
	ChannelOutputStreamBuffer buffer_;
};

} // namespace madc

#endif // __MADCDIS_CHANNEL_STREAM_H
