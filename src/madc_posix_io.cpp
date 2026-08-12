// The low-level IO owner — one definition per function, per-platform BODIES
// (the definition-counting gates key on single signatures:
// check-datachannel-write-owner.sh). Win32 bridges CRT fds to HANDLEs via
// _get_osfhandle where the Win32 API is the only expression.
#include "madc_posix_io.h"

#include <cerrno>
#include <cstdlib>
#ifdef _WIN32
#include <cstdio>
#include <cstring>
#include <io.h>
#include <limits.h>
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace madc {
namespace detail {

bool set_fd_close_on_exec(int fd)
{
#ifdef _WIN32
	// Close-on-exec == not inherited by CreateProcess children.
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if ( h == INVALID_HANDLE_VALUE )
		return false;
	return SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0) != 0;
#else
	int flags = ::fcntl(fd, F_GETFD);
	return flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
#endif
}

ssize_t write_fd_without_sigpipe(int fd, const void *buffer, std::size_t size)
{
#ifdef _WIN32
	// Windows has no SIGPIPE: a plain write already has the "report
	// EPIPE, don't kill the process" contract the POSIX body builds.
	unsigned int chunk = size > (std::size_t)INT_MAX ? (unsigned int)INT_MAX
							 : (unsigned int)size;
	return ::_write(fd, buffer, chunk);
#else
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
#endif
}

std::string resolve_real_path(const char *path)
{
#ifdef _WIN32
	// GetFullPathName normalizes (absolute, `.`/`..` collapsed) without
	// touching the filesystem — no existence check and no link
	// resolution, both permitted by the header contract (consistent
	// spellings, not link identity).
	char buf[MAX_PATH];
	DWORD n = GetFullPathNameA(path, sizeof(buf), buf, NULL);
	if ( n == 0 || n >= sizeof(buf) )
		return std::string();
	return std::string(buf, n);
#else
	char *rp = ::realpath(path, NULL);
	if ( !rp )
		return std::string();
	std::string out(rp);
	::free(rp);
	return out;
#endif
}

std::string get_host_name()
{
#ifdef _WIN32
	// GetComputerNameEx, deliberately not winsock gethostname — callers
	// run at static-init time, before any WSAStartup (header contract).
	char buf[256];
	DWORD n = sizeof(buf);
	if ( !GetComputerNameExA(ComputerNameDnsHostname, buf, &n) )
		return std::string();
	return std::string(buf, n);
#else
	char buf[256];
	if ( ::gethostname(buf, sizeof(buf) - 1) != 0 )
		return std::string();
	buf[sizeof(buf) - 1] = '\0';
	return std::string(buf);
#endif
}

ssize_t pread_fd(int fd, void *buffer, std::size_t size, long long offset)
{
#ifdef _WIN32
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if ( h == INVALID_HANDLE_VALUE )
		return -1;
	OVERLAPPED ov;
	std::memset(&ov, 0, sizeof(ov));
	ov.Offset = (DWORD)(offset & 0xffffffff);
	ov.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
	DWORD got = 0;
	if ( !ReadFile(h, buffer, (DWORD)size, &got, &ov) )
	{
		if ( GetLastError() == ERROR_HANDLE_EOF )
			return 0;
		errno = EIO;
		return -1;
	}
	return (ssize_t)got;
#else
	ssize_t result;
	do
		result = ::pread(fd, buffer, size, (off_t)offset);
	while ( result < 0 && errno == EINTR );
	return result;
#endif
}

ssize_t pwrite_fd(int fd, const void *buffer, std::size_t size,
		  long long offset)
{
#ifdef _WIN32
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if ( h == INVALID_HANDLE_VALUE )
		return -1;
	OVERLAPPED ov;
	std::memset(&ov, 0, sizeof(ov));
	ov.Offset = (DWORD)(offset & 0xffffffff);
	ov.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
	DWORD put = 0;
	if ( !WriteFile(h, buffer, (DWORD)size, &put, &ov) )
	{
		errno = EIO;
		return -1;
	}
	return (ssize_t)put;
#else
	ssize_t result;
	do
		result = ::pwrite(fd, buffer, size, (off_t)offset);
	while ( result < 0 && errno == EINTR );
	return result;
#endif
}

bool open_string_capture(StringCapture &cap)
{
#ifdef _WIN32
	// No memory-backed FILE* exists on Windows; capture through a
	// delete-on-close temp file. NOT tmpfile(): the MS CRT's tmpfile
	// creates in the drive root and fails for non-elevated users. The
	// "D" fopen flag deletes on close even on abnormal exit.
	cap.buf = NULL;
	cap.len = 0;
	cap.f = NULL;
	char *name = ::_tempnam(NULL, "madc");
	if ( !name )
		return false;
	cap.f = ::fopen(name, "w+bTD");
	::free(name);
	return cap.f != NULL;
#else
	cap.buf = NULL;
	cap.len = 0;
	cap.f = ::open_memstream(&cap.buf, &cap.len);
	return cap.f != NULL;
#endif
}

std::string finish_string_capture(StringCapture &cap)
{
#ifdef _WIN32
	if ( !cap.f )
		return std::string();
	std::string s;
	long end = ( ::fflush(cap.f) == 0 && ::fseek(cap.f, 0, SEEK_END) == 0 )
		 ? ::ftell(cap.f) : -1;
	if ( end > 0 && ::fseek(cap.f, 0, SEEK_SET) == 0 )
	{
		s.resize((std::size_t)end);
		std::size_t got = ::fread(&s[0], 1, (std::size_t)end, cap.f);
		s.resize(got);
	}
	::fclose(cap.f);	// "D" flag: the backing file deletes here
	cap.f = NULL;
	return s;
#else
	if ( !cap.f )
		return std::string();
	::fclose(cap.f);
	std::string s = cap.buf ? std::string(cap.buf, cap.len) : std::string();
	::free(cap.buf);
	cap.f = NULL;
	cap.buf = NULL;
	cap.len = 0;
	return s;
#endif
}

void *map_file_readonly(const char *path, std::size_t &length)
{
#ifdef _WIN32
	length = 0;
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
			       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if ( f == INVALID_HANDLE_VALUE )
		return NULL;
	LARGE_INTEGER sz;
	if ( !GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 )
	{
		CloseHandle(f);
		return NULL;
	}
	HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
	CloseHandle(f);	// the mapping holds its own reference
	if ( !m )
		return NULL;
	void *view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
	CloseHandle(m);	// the view holds its own reference
	if ( !view )
		return NULL;
	length = (std::size_t)sz.QuadPart;
	return view;
#else
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
#endif
}

} // namespace detail
} // namespace madc
