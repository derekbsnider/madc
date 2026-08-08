#include "madc_posix_io.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
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

} // namespace detail
} // namespace madc
