// madc_session_stdin.cpp — Send EOF's platform halves (plan
// docs/plans/madc-repl-thonny-plan-2026-09-24.md §41.12a); the contract is
// include/madc_session_stdin.h's. The pipe itself is Process's
// (Process::renew_stdin): this unit only carries its read end to the backend.


#include "madc_session_stdin.h"
#include "madcdis/process.h"

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <fcntl.h>

namespace madc {

SessionStdinHandOff::SessionStdinHandOff() : client_end_(-1), backend_end_(-1) {}

SessionStdinHandOff::~SessionStdinHandOff()
{
    close();
}

bool SessionStdinHandOff::open()
{
    return true;
}

int SessionStdinHandOff::backend_end() const
{
    return -1;
}

void SessionStdinHandOff::started() {}

void SessionStdinHandOff::in_backend() {}

bool SessionStdinHandOff::renew(Process &p, uint64_t &handle)
{
    intptr_t in_backend = 0;
    if ( !p.renew_stdin(in_backend) )
	return false;
    handle = (uint64_t)in_backend;
    return true;
}

void SessionStdinHandOff::close() {}

bool session_stdin_take(int, uint64_t handle)
{
    if ( handle == 0 )
	return false;
    int fd = ::_open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if ( fd < 0 )
    {
	CloseHandle((HANDLE)(uintptr_t)handle);
	return false;
    }
    const bool moved = ::_dup2(fd, 0) == 0;
    ::_close(fd);
    if ( !moved )
	return false;
    SetStdHandle(STD_INPUT_HANDLE, (HANDLE)::_get_osfhandle(0));
    return true;
}

} // namespace madc

#else

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "madc_datachannel_internal.h"	// create_socket_pair: close-on-exec ends

namespace {

// One fd across a unix socket: one byte of payload carries it.
bool send_fd(int sock, int fd)
{
    char byte = 0;
    struct iovec iov;
    iov.iov_base = &byte;
    iov.iov_len = 1;
    union { struct cmsghdr h; char space[CMSG_SPACE(sizeof(int))]; } ctl;
    memset(&ctl, 0, sizeof(ctl));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.space;
    msg.msg_controllen = sizeof(ctl.space);
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    ssize_t n;
    while ( (n = ::sendmsg(sock, &msg, 0)) < 0 && errno == EINTR )
	;
    return n == 1;
}

// The fd send_fd sent, or -1.
int recv_fd(int sock)
{
    char byte = 0;
    struct iovec iov;
    iov.iov_base = &byte;
    iov.iov_len = 1;
    union { struct cmsghdr h; char space[CMSG_SPACE(sizeof(int))]; } ctl;
    memset(&ctl, 0, sizeof(ctl));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.space;
    msg.msg_controllen = sizeof(ctl.space);
    ssize_t n;
    while ( (n = ::recvmsg(sock, &msg, 0)) < 0 && errno == EINTR )
	;
    if ( n != 1 )
	return -1;
    for ( struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c) )
	if ( c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS
	  && c->cmsg_len == CMSG_LEN(sizeof(int)) )
	{
	    int fd;
	    memcpy(&fd, CMSG_DATA(c), sizeof(int));
	    return fd;
	}
    return -1;
}

void close_end(int &fd)
{
    if ( fd >= 0 )
	::close(fd);
    fd = -1;
}

} // namespace

namespace madc {

SessionStdinHandOff::SessionStdinHandOff() : client_end_(-1), backend_end_(-1) {}

SessionStdinHandOff::~SessionStdinHandOff()
{
    close();
}

bool SessionStdinHandOff::open()
{
    close();
    int sv[2];
    // Close-on-exec: a program the backend execs keeps neither end.
    if ( !detail::create_socket_pair(AF_UNIX, SOCK_STREAM, 0, sv) )
	return false;
    client_end_ = sv[0];
    backend_end_ = sv[1];
    return true;
}

int SessionStdinHandOff::backend_end() const
{
    return backend_end_;
}

void SessionStdinHandOff::started()
{
    close_end(backend_end_);
}

void SessionStdinHandOff::in_backend()
{
    close_end(client_end_);
}

bool SessionStdinHandOff::renew(Process &p, uint64_t &handle)
{
    intptr_t fresh = -1;
    if ( client_end_ < 0 || !p.renew_stdin(fresh) )
	return false;
    int fd = (int)fresh;
    const bool sent = send_fd(client_end_, fd);
    close_end(fd);		// the backend holds it now
    handle = 0;
    return sent;
}

void SessionStdinHandOff::close()
{
    close_end(client_end_);
    close_end(backend_end_);
}

bool session_stdin_take(int socket_end, uint64_t)
{
    if ( socket_end < 0 )
	return false;
    int fd = recv_fd(socket_end);
    if ( fd < 0 )
	return false;
    const bool moved = ::dup2(fd, STDIN_FILENO) >= 0;
    ::close(fd);
    if ( !moved )
	return false;
    return true;
}

} // namespace madc

#endif
