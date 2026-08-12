#include "madcdis/datachannel.h"
#include "madc_datachannel_internal.h"
#include "madc_posix_io.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace madc {
namespace detail {

void set_channel_error(error *err, const std::string &message)
{
	if ( err )
		*err = error(error::severity::error, error::phase::runtime, // ERROR-COMPOSER-OWNER
			     message);
}

void set_channel_error(error *err, const std::string &operation,
		       const std::string &detail)
{
	set_channel_error(err, operation + ": " + detail);
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

class FileDataChannel : public DataChannel, public SeekableDataChannel
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
		ssize_t result = detail::read_fd(fd_, buffer, capacity);
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

	bool size(uint64_t &out, error *err = nullptr) override
	{
		out = 0;
		if ( !ensure_seekable("file size failed", err) )
			return false;
		struct stat st;
		if ( ::fstat(fd_, &st) != 0 )
		{
			set_channel_errno(err, "file size failed", path_);
			return false;
		}
		out = static_cast<uint64_t>(st.st_size);
		return true;
	}

	bool seek(uint64_t offset, error *err = nullptr) override
	{
		if ( !ensure_seekable("file seek failed", err) )
			return false;
		if ( ::lseek(fd_, static_cast<off_t>(offset), SEEK_SET) < 0 )
		{
			set_channel_errno(err, "file seek failed", path_);
			return false;
		}
		return true;
	}

	bool read_at(uint64_t offset, void *buffer, std::size_t capacity,
		     std::size_t &bytes_read, error *err = nullptr) override
	{
		bytes_read = 0;
		if ( !ensure_seekable("file read_at failed", err) )
			return false;
		if ( !capabilities_.read )
		{
			set_channel_error(err, "file read_at failed",
					  "channel is not readable");
			return false;
		}
		ssize_t result = detail::pread_fd(fd_, buffer, capacity,
						  (long long)offset);
		if ( result < 0 )
		{
			set_channel_errno(err, "file read_at failed", path_);
			return false;
		}
		bytes_read = static_cast<std::size_t>(result);
		return true;
	}

	bool write_at(uint64_t offset, const void *buffer, std::size_t size,
		      std::size_t &bytes_written, error *err = nullptr) override
	{
		bytes_written = 0;
		if ( !ensure_seekable("file write_at failed", err) )
			return false;
		if ( !capabilities_.write )
		{
			set_channel_error(err, "file write_at failed",
					  "channel is not writable");
			return false;
		}
		ssize_t result = detail::pwrite_fd(fd_, buffer, size,
						   (long long)offset);
		if ( result < 0 )
		{
			set_channel_errno(err, "file write_at failed", path_);
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
	bool ensure_seekable(const std::string &operation, error *err) const
	{
		if ( capabilities_.seek && fd_ >= 0 )
			return true;
		set_channel_error(err, operation, "channel is not seekable");
		return false;
	}

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
		return detail::open_file_channel(source.path(), mode, err);
	}
};

} // namespace

namespace detail {

std::unique_ptr<DataChannel> open_file_channel(const std::string &path,
					       ChannelOpenMode mode,
					       error *err)
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

	// O_CLOEXEC is POSIX-2008 (Linux and darwin both have it):
	// close-on-exec is atomic at open, no post-hoc fcntl window.
	flags |= O_CLOEXEC;

	int fd;
	do
		fd = ::open(path.c_str(), flags, 0666);
	while ( fd < 0 && errno == EINTR );
	if ( fd < 0 )
	{
		set_channel_errno(err, "file channel open failed", path);
		return std::unique_ptr<DataChannel>();
	}

	// The seek capability is per-instance truth: only a regular file is
	// random-access. A FIFO or device opened through the file/pipe scheme
	// keeps seek=false and the seekable surface refuses cleanly. An
	// O_APPEND channel is a sequential appender by construction (Linux
	// pwrite ignores the offset on O_APPEND), so it never claims seek.
	struct stat st;
	capabilities.seek = mode != ChannelOpenMode::append
		&& ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode);

	return std::unique_ptr<DataChannel>(
		new FileDataChannel(fd, path, capabilities));
}

} // namespace detail

SeekableDataChannel *seekable_surface(DataChannel *channel)
{
	if ( !channel || !channel->capabilities().seek )
		return nullptr;
	return dynamic_cast<SeekableDataChannel *>(channel);
}

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

bool copy_channel(DataChannel &source, DataChannel *destination,
		  std::size_t &byte_count, error *err)
{
	unsigned char buffer[16384];
	for ( ;; )
	{
		std::size_t count = 0;
		if ( !source.read(buffer, sizeof(buffer), count, err) )
			return false;
		if ( count == 0 )
			return destination ? destination->flush(err) : true;
		byte_count += count;
		if ( destination && !write_all(*destination, buffer, count, err) )
			return false;
	}
}

bool copy_channel(DataChannel &source, DataChannel &destination, error *err)
{
	std::size_t byte_count = 0;
	return copy_channel(source, &destination, byte_count, err);
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
	capabilities.seek = !read_closed_ || !write_closed_;
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
	// seek() may place the position past the end (file semantics); a
	// read there is EOF, not an underflowing size subtraction.
	std::size_t available = read_position_ < bytes_.size()
		? bytes_.size() - read_position_ : 0;
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

bool MemoryDataChannel::size(uint64_t &out, error *err)
{
	(void)err;
	out = bytes_.size();
	return true;
}

bool MemoryDataChannel::seek(uint64_t offset, error *err)
{
	if ( read_closed_ && write_closed_ )
	{
		set_channel_error(err, "memory seek failed", "channel is closed");
		return false;
	}
	read_position_ = static_cast<std::size_t>(offset);
	return true;
}

bool MemoryDataChannel::read_at(uint64_t offset, void *buffer,
				std::size_t capacity, std::size_t &bytes_read,
				error *err)
{
	bytes_read = 0;
	if ( read_closed_ )
	{
		set_channel_error(err, "memory read_at failed", "read side is closed");
		return false;
	}
	if ( offset >= bytes_.size() )
		return true;
	std::size_t available = bytes_.size() - static_cast<std::size_t>(offset);
	bytes_read = std::min(capacity, available);
	if ( bytes_read )
		std::memcpy(buffer, &bytes_[static_cast<std::size_t>(offset)],
			    bytes_read);
	return true;
}

bool MemoryDataChannel::write_at(uint64_t offset, const void *buffer,
				 std::size_t size, std::size_t &bytes_written,
				 error *err)
{
	bytes_written = 0;
	if ( write_closed_ )
	{
		set_channel_error(err, "memory write_at failed", "write side is closed");
		return false;
	}
	if ( size == 0 )
		return true;
	std::size_t end = static_cast<std::size_t>(offset) + size;
	if ( end > bytes_.size() )
		bytes_.resize(end, 0);
	std::memcpy(&bytes_[static_cast<std::size_t>(offset)], buffer, size);
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
	detail::register_exec_channel_factory(*this);
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
