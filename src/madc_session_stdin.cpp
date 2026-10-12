// madc_session_stdin.cpp — Send EOF's platform halves (plan
// docs/plans/madc-repl-thonny-plan-2026-09-24.md §41.12a); the contract is
// include/madc_session_stdin.h's. The pipe itself is Process's
// (Process::renew_stdin): this unit only carries its read end to the backend.


#include "madc_session_stdin.h"
#include "madcdis/process.h"

#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <fcntl.h>

namespace madc {

// The UCRT's view of a FILE's buffer (corecrt; no mingw header declares it):
// looked up at run time, so a C runtime without it (an older wine) costs the
// hand-back, never the start.
typedef int (__cdecl *stream_buffer_pointers_fn)(FILE *, char ***, char ***, int **);

std::string session_stdin_unread()
{
    if ( _isatty(_fileno(stdin)) )
	return std::string();		// a console delivers whole lines
    static const stream_buffer_pointers_fn pointers =
	(stream_buffer_pointers_fn)(void *)GetProcAddress(
	    GetModuleHandleA("ucrtbase.dll"), "_get_stream_buffer_pointers");
    char **base = 0, **ptr = 0;
    int *count = 0;
    if ( !pointers || pointers(stdin, &base, &ptr, &count) != 0 || !count
	 || *count <= 0 )
	return std::string();
    std::string unread;
    for ( int n = *count; n > 0; --n )
    {
	int c = getc(stdin);		// buffered: no read reaches the OS
	if ( c == EOF )
	    break;
	unread += (char)c;
    }
    return unread;
}

bool shared_stdin_input::fill()
{
    if ( !back_.empty() )
    {
	current_ = back_.front();
	back_.pop_front();
	return true;
    }
    DWORD got = 0;
    if ( !ReadFile(GetStdHandle(STD_INPUT_HANDLE), &current_, 1, &got, NULL)
	 || got != 1 )
	return false;
    return true;
}

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

std::string session_stdin_unread()
{
    return std::string();		// read byte by byte: nothing read ahead
}

bool shared_stdin_input::fill()
{
    if ( !back_.empty() )
    {
	current_ = back_.front();
	back_.pop_front();
	return true;
    }
    for ( ;; )
    {
	ssize_t got = ::read(0, &current_, 1);
	if ( got == 1 )
	    return true;
	if ( got < 0 && errno == EINTR )
	    continue;
	return false;
    }
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

namespace madc {

// The byte fill() read is the get area's one character; uflow consumes it.
shared_stdin_input::int_type shared_stdin_input::underflow()
{
    if ( gptr() && gptr() < egptr() )
	return traits_type::to_int_type(*gptr());
    if ( !fill() )
	return traits_type::eof();
    setg(&current_, &current_, &current_ + 1);
    return traits_type::to_int_type(current_);
}

shared_stdin_input::int_type shared_stdin_input::uflow()
{
    int_type c = underflow();
    if ( c != traits_type::eof() )
	gbump(1);
    return c;
}

// putback: the byte goes before everything not read yet (the unread byte in
// the get area, if any, then back_).
shared_stdin_input::int_type shared_stdin_input::pbackfail(int_type c)
{
    if ( traits_type::eq_int_type(c, traits_type::eof()) )
	return traits_type::eof();
    if ( gptr() && gptr() < egptr() )
	back_.push_front(*gptr());	// the pending byte stays after it
    back_.push_front(traits_type::to_char_type(c));
    setg(&current_, &current_, &current_);	// empty: the next read is back_'s
    return c;
}

} // namespace madc
