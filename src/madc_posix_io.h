#ifndef __MADC_POSIX_IO_H
#define __MADC_POSIX_IO_H 1

#include <cstddef>
#include <cstdio>
#include <ctime>
#include <string>
#include <sys/types.h>

namespace madc {
namespace detail {

bool set_fd_close_on_exec(int fd);
ssize_t write_fd_without_sigpipe(int fd, const void *buffer, std::size_t size);

// Read up to `size` bytes from an fd (POSIX read, EINTR-retrying). The
// Win32 arm is one _read capped at INT_MAX per call (the CRT takes an
// unsigned int); both arms return bytes read, 0 at EOF, -1 with errno set.
// Callers already handle short reads — every channel contract allows them.
ssize_t read_fd(int fd, void *buffer, std::size_t size);

// Canonical absolute form of `path` (POSIX realpath: resolves symlinks,
// `.` and `..`); empty string when it does not resolve. The Win32 arm
// (GetFullPathName) normalizes without following links — consumers use
// this for CONSISTENT path spellings, never for link identity.
std::string resolve_real_path(const char *path);

// Thread-safe local time (POSIX localtime_r). The Win32 arm is the MS
// localtime_s — NOTE the two spell their argument orders opposite ways,
// which is exactly why call sites go through this owner. False on failure,
// `out` untouched then.
bool local_time(time_t t, struct tm &out);

// This host's name (POSIX gethostname); empty string on failure. The
// Win32 arm rides GetComputerNameEx, NOT winsock's gethostname — callers
// run at static-init time, before any WSAStartup.
std::string get_host_name();

// Absolute SEEK_SET reposition; the new offset, or -1 with errno set.
// 64-bit on both arms — mingw's off_t/lseek are 32-bit, so the Win32 arm
// rides _lseeki64 (a plain ::lseek would truncate past 2GB).
long long seek_fd(int fd, long long offset);

// Byte size of the open fd (fstat); -1 with errno set on failure. 64-bit
// on both arms — mingw's struct stat st_size is 32-bit, so the Win32 arm
// rides _fstat64.
long long fd_size(int fd);

// True when the open fd is a regular file (the channel seekability
// predicate). The Win32 arm uses the 64-bit stat: the 32-bit _fstat FAILS
// outright on >4GB files, which would silently demote a big regular file
// to non-seekable.
bool fd_is_regular_file(int fd);

// Positioned read/write on an fd (POSIX pread/pwrite): EINTR-retrying,
// never moves the file pointer; -1 with errno on failure. The Win32 arm
// rides ReadFile/WriteFile with an OVERLAPPED offset behind these
// signatures.
ssize_t pread_fd(int fd, void *buffer, std::size_t size, long long offset);
ssize_t pwrite_fd(int fd, const void *buffer, std::size_t size,
		  long long offset);

// String-capture stream: a FILE* whose writes accumulate in memory;
// finish_string_capture closes it and returns everything written (empty
// string if the capture never opened). POSIX arm = open_memstream; the
// Win32 arm rides a tmpfile read-back (Windows FILE* has no memory hook).
// One owner so FILE*-shaped emitters stay platform-agnostic.
struct StringCapture
{
	FILE *f = NULL;
	char *buf = NULL;
	std::size_t len = 0;
};
bool open_string_capture(StringCapture &cap);
std::string finish_string_capture(StringCapture &cap);

// Map an entire file read-only (POSIX mmap PROT_READ/MAP_PRIVATE). NULL on
// any failure, including an empty file; the byte count comes back through
// `length`. Lifetime belongs to the caller — cir_freeze's forest image
// deliberately keeps its mapping for the process lifetime. The one mapping
// owner: the Win32 backend (CreateFileMapping/MapViewOfFile) lands behind
// this signature only.
void *map_file_readonly(const char *path, std::size_t &length);

#ifdef _WIN32
// GetLastError code -> trimmed FormatMessage text ("Windows error N" when
// the system has no message). The one Win32-error formatter — error-path
// composition (dlerror emulation, process errors) layers on top of it.
std::string win_error_text(unsigned long code);

// One line to the Windows debug channel (OutputDebugStringA) — the
// syslog-analogue sink target, visible in a debugger / DebugView. The
// caller composes the line (ident, level, message); this is transport only.
void debug_log_line(const std::string &line);
#endif

} // namespace detail
} // namespace madc

#endif // __MADC_POSIX_IO_H
