#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

bool write_all(int fd, const void *buffer, std::size_t size)
{
	const char *next = static_cast<const char *>(buffer);
	while ( size )
	{
		ssize_t count = ::write(fd, next, size);
		if ( count < 0 && errno == EINTR )
			continue;
		if ( count <= 0 )
			return false;
		next += count;
		size -= static_cast<std::size_t>(count);
	}
	return true;
}

int echo_input(bool copy_to_stderr)
{
	char buffer[4096];
	for ( ;; )
	{
		ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
		if ( count < 0 && errno == EINTR )
			continue;
		if ( count < 0 )
			return 90;
		if ( count == 0 )
			return 0;
		if ( !write_all(STDOUT_FILENO, buffer, static_cast<std::size_t>(count)) )
			return 91;
		if ( copy_to_stderr
		  && !write_all(STDERR_FILENO, buffer, static_cast<std::size_t>(count)) )
			return 92;
	}
}

int report_open_descriptor_count()
{
	int count = 0;
	for ( int fd = 3; fd < 256; ++fd )
	{
		errno = 0;
		if ( ::fcntl(fd, F_GETFD) >= 0 || errno != EBADF )
			++count;
	}
	std::string result = std::to_string(count) + "\n";
	return write_all(STDOUT_FILENO, result.data(), result.size()) ? 0 : 95;
}

} // namespace

int main(int argc, char **argv)
{
	if ( argc < 2 )
		return 64;
	std::string mode(argv[1]);
	if ( mode == "argv" )
	{
		for ( int i = 2; i < argc; ++i )
		{
			std::string value(argv[i]);
			std::string line = std::to_string(value.size()) + ":" + value + "\n";
			if ( !write_all(STDOUT_FILENO, line.data(), line.size()) )
				return 93;
		}
		return 0;
	}
	if ( mode == "echo" )
	{
		int result = echo_input(false);
		if ( result != 0 )
			return result;
		if ( argc > 2 && !write_all(STDERR_FILENO, argv[2], std::strlen(argv[2])) )
			return 94;
		return argc > 3 ? std::atoi(argv[3]) : 0;
	}
	if ( mode == "pump" )
		return echo_input(true);
	if ( mode == "fd-count" )
		return report_open_descriptor_count();
	return 65;
}
