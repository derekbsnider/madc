#include "madcdis/datachannel.h"
#include "madc_datachannel_internal.h"
#include "madc_posix_io.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace madc {
namespace {

enum class SocketSemantics
{
	byte_stream,
	datagram
};

int create_socket(int domain, int socket_type, int protocol)
{
#ifdef SOCK_CLOEXEC
	// Atomic close-on-exec at creation: no window for a concurrent
	// fork+exec in a threaded embedding host to leak the fd.
	return ::socket(domain, socket_type | SOCK_CLOEXEC, protocol);
#else
	// Portable fallback (darwin has no SOCK_CLOEXEC): post-hoc owner.
	int fd = ::socket(domain, socket_type, protocol);
	if ( fd < 0 )
		return -1;
	if ( detail::set_fd_close_on_exec(fd) )
		return fd;
	int number = errno;
	::close(fd);
	errno = number;
	return -1;
#endif
}

bool socket_capabilities(ChannelOpenMode mode, SocketSemantics semantics,
			 ChannelCapabilities &capabilities, error *err)
{
	switch ( mode )
	{
	case ChannelOpenMode::read:
		capabilities.read = true;
		break;
	case ChannelOpenMode::write:
		capabilities.write = true;
		break;
	case ChannelOpenMode::read_write:
		capabilities.read = true;
		capabilities.write = true;
		break;
	case ChannelOpenMode::append:
		detail::set_channel_error(err, "socket channel open failed",
					  "append mode is not meaningful for sockets");
		return false;
	}
	capabilities.half_close = semantics == SocketSemantics::byte_stream;
	return true;
}

bool split_network_endpoint(const DataSource &source, std::string &host,
			    std::string &service, error *err)
{
	const std::string &authority = source.authority();
	if ( authority.empty() )
	{
		detail::set_channel_error(err, "socket channel open failed",
					  source.uri() + ": missing host and port");
		return false;
	}
	if ( source.path() != "/" )
	{
		detail::set_channel_error(err, "socket channel open failed",
					  source.uri() + ": raw socket URI cannot contain a path");
		return false;
	}

	if ( authority[0] == '[' )
	{
		std::size_t bracket = authority.find(']');
		if ( bracket == std::string::npos || bracket + 1 >= authority.size()
		  || authority[bracket + 1] != ':' )
		{
			detail::set_channel_error(err, "socket channel open failed",
						  source.uri() + ": expected [host]:port");
			return false;
		}
		host = authority.substr(1, bracket - 1);
		service = authority.substr(bracket + 2);
	}
	else
	{
		std::size_t separator = authority.rfind(':');
		if ( separator == std::string::npos
		  || authority.find(':') != separator )
		{
			detail::set_channel_error(err, "socket channel open failed",
						  source.uri() + ": expected host:port");
			return false;
		}
		host = authority.substr(0, separator);
		service = authority.substr(separator + 1);
	}

	if ( host.empty() || service.empty() )
	{
		detail::set_channel_error(err, "socket channel open failed",
					  source.uri() + ": host and port must be non-empty");
		return false;
	}
	return true;
}

int connect_network_socket(const DataSource &source, int socket_type,
			   int protocol, error *err)
{
	std::string host;
	std::string service;
	if ( !split_network_endpoint(source, host, service, err) )
		return -1;

	addrinfo hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = socket_type;
	hints.ai_protocol = protocol;
	addrinfo *addresses = nullptr;
	int lookup = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
	if ( lookup != 0 )
	{
		detail::set_channel_error(err, "socket address lookup failed",
					  source.authority() + ": " + ::gai_strerror(lookup));
		return -1;
	}

	int fd = -1;
	int last_error = ECONNREFUSED;
	for ( addrinfo *address = addresses; address; address = address->ai_next )
	{
		fd = create_socket(address->ai_family, address->ai_socktype,
				   address->ai_protocol);
		if ( fd < 0 )
		{
			last_error = errno;
			continue;
		}
		if ( ::connect(fd, address->ai_addr, address->ai_addrlen) == 0 )
			break;
		last_error = errno;
		::close(fd);
		fd = -1;
	}
	::freeaddrinfo(addresses);
	if ( fd < 0 )
	{
		errno = last_error;
		detail::set_channel_errno(err, "socket connect failed", source.authority());
	}
	return fd;
}

int connect_unix_socket(const DataSource &source, error *err)
{
	const std::string &path = source.path();
	if ( path.empty() || path == "/" )
	{
		detail::set_channel_error(err, "unix socket connect failed",
					  source.uri() + ": missing socket path");
		return -1;
	}

	sockaddr_un address;
	std::memset(&address, 0, sizeof(address));
	if ( path.size() >= sizeof(address.sun_path) )
	{
		detail::set_channel_error(err, "unix socket connect failed",
					  path + ": socket path is too long");
		return -1;
	}
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

	int fd = create_socket(AF_UNIX, SOCK_STREAM, 0);
	if ( fd < 0 )
	{
		detail::set_channel_errno(err, "unix socket creation failed", path);
		return -1;
	}
	socklen_t address_size = static_cast<socklen_t>(
		offsetof(sockaddr_un, sun_path) + path.size() + 1);
	if ( ::connect(fd, reinterpret_cast<sockaddr *>(&address), address_size) != 0 )
	{
		int number = errno;
		::close(fd);
		errno = number;
		detail::set_channel_errno(err, "unix socket connect failed", path);
		return -1;
	}
	return fd;
}

class SocketDataChannel : public DataChannel
{
public:
	SocketDataChannel(int fd, const std::string &scheme,
			  const std::string &endpoint,
			  const ChannelCapabilities &capabilities,
			  SocketSemantics semantics)
		: fd_(fd), scheme_(scheme), endpoint_(endpoint),
		  capabilities_(capabilities), semantics_(semantics)
	{}

	~SocketDataChannel() override { close(); }

	const char *name() const override { return scheme_.c_str(); }
	ChannelCapabilities capabilities() const override { return capabilities_; }

	bool read(void *buffer, std::size_t capacity, std::size_t &bytes_read,
		  error *err = nullptr) override
	{
		if ( semantics_ == SocketSemantics::datagram )
		{
			if ( !receive_datagram(buffer, capacity, bytes_read, err) )
				return false;
			if ( bytes_read == 0 )
			{
				detail::set_channel_error(
					err, scheme_ + " read failed",
					"a zero-length datagram requires receive_datagram()");
				return false;
			}
			return true;
		}

		bytes_read = 0;
		if ( fd_ < 0 || !capabilities_.read )
		{
			detail::set_channel_error(err, scheme_ + " read failed",
						  "channel is not readable");
			return false;
		}
		ssize_t result;
		do
			result = ::recv(fd_, buffer, capacity, 0);
		while ( result < 0 && errno == EINTR );
		if ( result < 0 )
		{
			detail::set_channel_errno(err, scheme_ + " read failed", endpoint_);
			return false;
		}
		bytes_read = static_cast<std::size_t>(result);
		return true;
	}

	bool write(const void *buffer, std::size_t size, std::size_t &bytes_written,
		   error *err = nullptr) override
	{
		if ( semantics_ == SocketSemantics::datagram )
			return send_datagram(buffer, size, bytes_written, err);

		bytes_written = 0;
		if ( fd_ < 0 || !capabilities_.write )
		{
			detail::set_channel_error(err, scheme_ + " write failed",
						  "channel is not writable");
			return false;
		}
		ssize_t result = detail::write_fd_without_sigpipe(fd_, buffer, size);
		if ( result < 0 )
		{
			detail::set_channel_errno(err, scheme_ + " write failed", endpoint_);
			return false;
		}
		bytes_written = static_cast<std::size_t>(result);
		return true;
	}

	void close_read() override
	{
		if ( fd_ < 0 || !capabilities_.read )
			return;
		if ( semantics_ == SocketSemantics::byte_stream )
			::shutdown(fd_, SHUT_RD);
		capabilities_.read = false;
		if ( !capabilities_.write )
			close();
	}

	void close_write() override
	{
		if ( fd_ < 0 || !capabilities_.write )
			return;
		if ( semantics_ == SocketSemantics::byte_stream )
			::shutdown(fd_, SHUT_WR);
		capabilities_.write = false;
		if ( !capabilities_.read )
			close();
	}

	void close() override
	{
		if ( fd_ >= 0 )
			::close(fd_);
		fd_ = -1;
		capabilities_.read = false;
		capabilities_.write = false;
	}

protected:
	bool receive_datagram(void *buffer, std::size_t capacity,
			      std::size_t &bytes_read, error *err = nullptr)
	{
		bytes_read = 0;
		if ( semantics_ != SocketSemantics::datagram )
		{
			detail::set_channel_error(err, scheme_ + " receive failed",
						  "channel is not datagram-oriented");
			return false;
		}
		if ( fd_ < 0 || !capabilities_.read )
		{
			detail::set_channel_error(err, scheme_ + " receive failed",
						  "channel is not readable");
			return false;
		}

		iovec vector;
		vector.iov_base = buffer;
		vector.iov_len = capacity;
		msghdr message;
		std::memset(&message, 0, sizeof(message));
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		ssize_t result;
		do
			result = ::recvmsg(fd_, &message, 0);
		while ( result < 0 && errno == EINTR );
		if ( result < 0 )
		{
			detail::set_channel_errno(err, scheme_ + " receive failed", endpoint_);
			return false;
		}
		if ( (message.msg_flags & MSG_TRUNC) != 0 )
		{
			detail::set_channel_error(err, scheme_ + " receive failed",
						  endpoint_ + ": datagram exceeds receive buffer");
			return false;
		}
		bytes_read = static_cast<std::size_t>(result);
		return true;
	}

	bool send_datagram(const void *buffer, std::size_t size,
			   std::size_t &bytes_written, error *err = nullptr)
	{
		bytes_written = 0;
		if ( semantics_ != SocketSemantics::datagram )
		{
			detail::set_channel_error(err, scheme_ + " send failed",
						  "channel is not datagram-oriented");
			return false;
		}
		if ( fd_ < 0 || !capabilities_.write )
		{
			detail::set_channel_error(err, scheme_ + " send failed",
						  "channel is not writable");
			return false;
		}

		ssize_t result;
		do
			result = ::send(fd_, buffer, size, 0);
		while ( result < 0 && errno == EINTR );
		if ( result < 0 )
		{
			detail::set_channel_errno(err, scheme_ + " send failed", endpoint_);
			return false;
		}
		if ( static_cast<std::size_t>(result) != size )
		{
			detail::set_channel_error(err, scheme_ + " send failed",
						  endpoint_ + ": partial datagram send");
			return false;
		}
		bytes_written = static_cast<std::size_t>(result);
		return true;
	}

private:
	int fd_;
	std::string scheme_;
	std::string endpoint_;
	ChannelCapabilities capabilities_;
	SocketSemantics semantics_;
};

class DatagramSocketDataChannel : public SocketDataChannel,
				  public DatagramDataChannel
{
public:
	DatagramSocketDataChannel(int fd, const std::string &scheme,
				  const std::string &endpoint,
				  const ChannelCapabilities &capabilities)
		: SocketDataChannel(fd, scheme, endpoint, capabilities,
				    SocketSemantics::datagram)
	{}

	bool receive_datagram(void *buffer, std::size_t capacity,
			      std::size_t &bytes_read, error *err = nullptr) override
	{
		return SocketDataChannel::receive_datagram(
			buffer, capacity, bytes_read, err);
	}

	bool send_datagram(const void *buffer, std::size_t size,
			   std::size_t &bytes_written, error *err = nullptr) override
	{
		return SocketDataChannel::send_datagram(
			buffer, size, bytes_written, err);
	}
};

class NetworkSocketChannelFactory : public DataChannelRegistry::Factory
{
public:
	NetworkSocketChannelFactory(const std::string &scheme, int socket_type,
				    int protocol, SocketSemantics semantics)
		: scheme_(scheme), socket_type_(socket_type), protocol_(protocol),
		  semantics_(semantics)
	{}

	std::unique_ptr<DataChannel> open(const DataSource &source,
					  ChannelOpenMode mode,
					  error *err = nullptr) const override
	{
		ChannelCapabilities capabilities;
		if ( !socket_capabilities(mode, semantics_, capabilities, err) )
			return std::unique_ptr<DataChannel>();
		int fd = connect_network_socket(source, socket_type_, protocol_, err);
		if ( fd < 0 )
			return std::unique_ptr<DataChannel>();
		if ( semantics_ == SocketSemantics::datagram )
			return std::unique_ptr<DataChannel>(new DatagramSocketDataChannel(
				fd, scheme_, source.authority(), capabilities));
		return std::unique_ptr<DataChannel>(new SocketDataChannel(
			fd, scheme_, source.authority(), capabilities, semantics_));
	}

private:
	std::string scheme_;
	int socket_type_;
	int protocol_;
	SocketSemantics semantics_;
};

class UnixSocketChannelFactory : public DataChannelRegistry::Factory
{
public:
	explicit UnixSocketChannelFactory(const std::string &scheme)
		: scheme_(scheme)
	{}

	std::unique_ptr<DataChannel> open(const DataSource &source,
					  ChannelOpenMode mode,
					  error *err = nullptr) const override
	{
		ChannelCapabilities capabilities;
		if ( !socket_capabilities(
				mode, SocketSemantics::byte_stream, capabilities, err) )
			return std::unique_ptr<DataChannel>();
		int fd = connect_unix_socket(source, err);
		if ( fd < 0 )
			return std::unique_ptr<DataChannel>();
		return std::unique_ptr<DataChannel>(new SocketDataChannel(
			fd, scheme_, source.path(), capabilities,
			SocketSemantics::byte_stream));
	}

private:
	std::string scheme_;
};

} // namespace

namespace detail {

void register_socket_channel_factories(DataChannelRegistry &registry)
{
	registry.register_factory(
		"tcp", std::unique_ptr<DataChannelRegistry::Factory>(
			       new NetworkSocketChannelFactory(
				       "tcp", SOCK_STREAM, IPPROTO_TCP,
				       SocketSemantics::byte_stream)));
	registry.register_factory(
		"udp", std::unique_ptr<DataChannelRegistry::Factory>(
			       new NetworkSocketChannelFactory(
				       "udp", SOCK_DGRAM, IPPROTO_UDP,
				       SocketSemantics::datagram)));
	registry.register_factory(
		"uds", std::unique_ptr<DataChannelRegistry::Factory>(
			       new UnixSocketChannelFactory("uds")));
	registry.register_factory(
		"unix", std::unique_ptr<DataChannelRegistry::Factory>(
				new UnixSocketChannelFactory("unix")));
}

} // namespace detail
} // namespace madc
