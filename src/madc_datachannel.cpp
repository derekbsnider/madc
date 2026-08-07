#include "madcdis/datachannel.h"
#include "madc_datachannel_internal.h"
#include "madc_posix_io.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <unistd.h>
#include <utility>

namespace madc {
namespace detail {

void set_channel_error(error *err, const std::string &operation,
		       const std::string &detail)
{
	if ( err )
		*err = error(error::severity::error, error::phase::runtime,
			     operation + ": " + detail);
}

void set_channel_errno(error *err, const std::string &operation,
		       const std::string &path)
{
	set_channel_error(err, operation,
			  path + ": " + std::string(std::strerror(errno)));
}

} // namespace detail
namespace {

using detail::set_channel_errno;
using detail::set_channel_error;

class FileDataChannel : public DataChannel
{
public:
	FileDataChannel(int fd, const std::string &path,
			ChannelCapabilities capabilities)
		: fd_(fd), path_(path), capabilities_(capabilities)
	{}

	~FileDataChannel() override { close(); }

	const char *name() const override { return "file"; }
	ChannelCapabilities capabilities() const override { return capabilities_; }

	bool read(void *buffer, std::size_t capacity, std::size_t &bytes_read,
		  error *err = nullptr) override
	{
		bytes_read = 0;
		if ( !capabilities_.read || fd_ < 0 )
		{
			set_channel_error(err, "file read failed", "channel is not readable");
			return false;
		}
		ssize_t result;
		do
			result = ::read(fd_, buffer, capacity);
		while ( result < 0 && errno == EINTR );
		if ( result < 0 )
		{
			set_channel_errno(err, "file read failed", path_);
			return false;
		}
		bytes_read = static_cast<std::size_t>(result);
		return true;
	}

	bool write(const void *buffer, std::size_t size, std::size_t &bytes_written,
		   error *err = nullptr) override
	{
		bytes_written = 0;
		if ( !capabilities_.write || fd_ < 0 )
		{
			set_channel_error(err, "file write failed", "channel is not writable");
			return false;
		}
		ssize_t result = detail::write_fd_without_sigpipe(fd_, buffer, size);
		if ( result < 0 )
		{
			set_channel_errno(err, "file write failed", path_);
			return false;
		}
		bytes_written = static_cast<std::size_t>(result);
		return true;
	}

	void close() override
	{
		if ( fd_ >= 0 )
			::close(fd_);
		fd_ = -1;
	}

private:
	int fd_;
	std::string path_;
	ChannelCapabilities capabilities_;
};

class FileChannelFactory : public DataChannelRegistry::Factory
{
public:
	std::unique_ptr<DataChannel> open(const DataSource &source,
					  ChannelOpenMode mode,
					  error *err = nullptr) const override
	{
		int flags = O_RDONLY;
		ChannelCapabilities capabilities;
		switch ( mode )
		{
		case ChannelOpenMode::read:
			capabilities.read = true;
			break;
		case ChannelOpenMode::write:
			flags = O_WRONLY | O_CREAT | O_TRUNC;
			capabilities.write = true;
			break;
		case ChannelOpenMode::read_write:
			flags = O_RDWR | O_CREAT;
			capabilities.read = true;
			capabilities.write = true;
			break;
		case ChannelOpenMode::append:
			flags = O_WRONLY | O_CREAT | O_APPEND;
			capabilities.write = true;
			break;
		}

		int fd;
		do
			fd = ::open(source.path().c_str(), flags, 0666);
		while ( fd < 0 && errno == EINTR );
		if ( fd < 0 )
		{
			set_channel_errno(err, "file channel open failed", source.path());
			return std::unique_ptr<DataChannel>();
		}
		if ( !detail::set_fd_close_on_exec(fd) )
		{
			int number = errno;
			::close(fd);
			errno = number;
			set_channel_errno(
				err, "file channel close-on-exec setup failed", source.path());
			return std::unique_ptr<DataChannel>();
		}
		return std::unique_ptr<DataChannel>(
			new FileDataChannel(fd, source.path(), capabilities));
	}
};

} // namespace

bool write_all(DataChannel &channel, const void *buffer, std::size_t size,
	       error *err)
{
	const unsigned char *next = static_cast<const unsigned char *>(buffer);
	std::size_t remaining = size;
	while ( remaining )
	{
		std::size_t written = 0;
		if ( !channel.write(next, remaining, written, err) )
			return false;
		if ( written == 0 )
		{
			set_channel_error(err, "channel write failed",
					  "write made no progress");
			return false;
		}
		next += written;
		remaining -= written;
	}
	return true;
}

bool copy_channel(DataChannel &source, DataChannel &destination, error *err)
{
	unsigned char buffer[16384];
	for ( ;; )
	{
		std::size_t count = 0;
		if ( !source.read(buffer, sizeof(buffer), count, err) )
			return false;
		if ( count == 0 )
			return destination.flush(err);
		if ( !write_all(destination, buffer, count, err) )
			return false;
	}
}

MemoryDataChannel::MemoryDataChannel()
	: read_position_(0), read_closed_(false), write_closed_(false)
{}

MemoryDataChannel::MemoryDataChannel(const std::vector<unsigned char> &bytes)
	: bytes_(bytes), read_position_(0), read_closed_(false), write_closed_(false)
{}

MemoryDataChannel::~MemoryDataChannel()
{
	close();
}

const char *MemoryDataChannel::name() const { return "memory"; }

ChannelCapabilities MemoryDataChannel::capabilities() const
{
	ChannelCapabilities capabilities;
	capabilities.read = !read_closed_;
	capabilities.write = !write_closed_;
	return capabilities;
}

bool MemoryDataChannel::read(void *buffer, std::size_t capacity,
			     std::size_t &bytes_read, error *err)
{
	bytes_read = 0;
	if ( read_closed_ )
	{
		set_channel_error(err, "memory read failed", "read side is closed");
		return false;
	}
	std::size_t available = bytes_.size() - read_position_;
	bytes_read = std::min(capacity, available);
	if ( bytes_read )
		std::memcpy(buffer, &bytes_[read_position_], bytes_read);
	read_position_ += bytes_read;
	return true;
}

bool MemoryDataChannel::write(const void *buffer, std::size_t size,
			      std::size_t &bytes_written, error *err)
{
	bytes_written = 0;
	if ( write_closed_ )
	{
		set_channel_error(err, "memory write failed", "write side is closed");
		return false;
	}
	if ( size == 0 )
		return true;
	const unsigned char *bytes = static_cast<const unsigned char *>(buffer);
	bytes_.insert(bytes_.end(), bytes, bytes + size);
	bytes_written = size;
	return true;
}

void MemoryDataChannel::close_read() { read_closed_ = true; }
void MemoryDataChannel::close_write() { write_closed_ = true; }

void MemoryDataChannel::close()
{
	read_closed_ = true;
	write_closed_ = true;
}

const std::vector<unsigned char> &MemoryDataChannel::bytes() const { return bytes_; }

void MemoryDataChannel::reset_read()
{
	read_position_ = 0;
	read_closed_ = false;
}

void MemoryDataChannel::clear()
{
	bytes_.clear();
	read_position_ = 0;
}

struct DataChannelRegistry::impl
{
	std::map<std::string, std::unique_ptr<Factory> > factories;
};

DataChannelRegistry::DataChannelRegistry()
	: _(new impl())
{
	register_factory("file", std::unique_ptr<Factory>(new FileChannelFactory()));
	register_factory("pipe", std::unique_ptr<Factory>(new FileChannelFactory()));
	detail::register_socket_channel_factories(*this);
}

DataChannelRegistry &DataChannelRegistry::instance()
{
	static DataChannelRegistry registry;
	return registry;
}

void DataChannelRegistry::register_factory(const std::string &scheme,
					   std::unique_ptr<Factory> factory)
{
	_->factories[scheme] = std::move(factory);
}

bool DataChannelRegistry::has_factory(const std::string &scheme) const
{
	return _->factories.count(scheme) != 0;
}

std::unique_ptr<DataChannel> DataChannelRegistry::open(
		const DataSource &source, ChannelOpenMode mode, error *err) const
{
	std::map<std::string, std::unique_ptr<Factory> >::const_iterator it =
		_->factories.find(source.scheme());
	if ( it == _->factories.end() || !it->second.get() )
	{
		set_channel_error(err, "channel open failed",
				  "no DataChannel provider is registered for scheme `"
				  + source.scheme() + "`");
		return std::unique_ptr<DataChannel>();
	}
	return it->second->open(source, mode, err);
}

std::vector<std::string> DataChannelRegistry::schemes() const
{
	std::vector<std::string> out;
	for ( std::map<std::string, std::unique_ptr<Factory> >::const_iterator it =
		  _->factories.begin(); it != _->factories.end(); ++it )
		out.push_back(it->first);
	return out;
}

} // namespace madc
