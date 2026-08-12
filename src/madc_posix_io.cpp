#include "madc_posix_io.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace madc {
namespace detail {

bool set_fd_close_on_exec(int fd)
{
	int flags = ::fcntl(fd, F_GETFD);
	return flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

ssize_t write_fd_without_sigpipe(int fd, const void *buffer, std::size_t size)
{
	sigset_t blocked;
	sigset_t previous;
	sigset_t pending;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGPIPE);
	int mask_error = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
	if ( mask_error != 0 )
	{
		errno = mask_error;
		return -1;
	}
	if ( sigpending(&pending) != 0 )
	{
		int number = errno;
		pthread_sigmask(SIG_SETMASK, &previous, nullptr);
		errno = number;
		return -1;
	}
	bool already_pending = sigismember(&pending, SIGPIPE) == 1;

	ssize_t result;
	do
		result = ::write(fd, buffer, size); // SIGPIPE-OWNER
	while ( result < 0 && errno == EINTR );
	int number = errno;

	if ( result < 0 && number == EPIPE && !already_pending )
	{
		int signal_number = 0;
		sigwait(&blocked, &signal_number);
	}
	pthread_sigmask(SIG_SETMASK, &previous, nullptr);
	errno = number;
	return result;
}

std::string resolve_real_path(const char *path)
{
	char *rp = ::realpath(path, NULL);
	if ( !rp )
		return std::string();
	std::string out(rp);
	::free(rp);
	return out;
}

std::string get_host_name()
{
	char buf[256];
	if ( ::gethostname(buf, sizeof(buf) - 1) != 0 )
		return std::string();
	buf[sizeof(buf) - 1] = '\0';
	return std::string(buf);
}

ssize_t pread_fd(int fd, void *buffer, std::size_t size, long long offset)
{
	ssize_t result;
	do
		result = ::pread(fd, buffer, size, (off_t)offset);
	while ( result < 0 && errno == EINTR );
	return result;
}

ssize_t pwrite_fd(int fd, const void *buffer, std::size_t size,
		  long long offset)
{
	ssize_t result;
	do
		result = ::pwrite(fd, buffer, size, (off_t)offset);
	while ( result < 0 && errno == EINTR );
	return result;
}

bool open_string_capture(StringCapture &cap)
{
	cap.buf = NULL;
	cap.len = 0;
	cap.f = ::open_memstream(&cap.buf, &cap.len);
	return cap.f != NULL;
}

std::string finish_string_capture(StringCapture &cap)
{
	if ( !cap.f )
		return std::string();
	::fclose(cap.f);
	std::string s = cap.buf ? std::string(cap.buf, cap.len) : std::string();
	::free(cap.buf);
	cap.f = NULL;
	cap.buf = NULL;
	cap.len = 0;
	return s;
}

void *map_file_readonly(const char *path, std::size_t &length)
{
	length = 0;
	int fd = ::open(path, O_RDONLY);
	if ( fd < 0 )
		return NULL;
	struct stat st;
	if ( ::fstat(fd, &st) != 0 || st.st_size <= 0 )
	{
		::close(fd);
		return NULL;
	}
	void *m = ::mmap(NULL, (std::size_t)st.st_size, PROT_READ, MAP_PRIVATE,
			 fd, 0);
	::close(fd);
	if ( m == MAP_FAILED )
		return NULL;
	length = (std::size_t)st.st_size;
	return m;
}

} // namespace detail
} // namespace madc
