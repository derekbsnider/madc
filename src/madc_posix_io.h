#ifndef __MADC_POSIX_IO_H
#define __MADC_POSIX_IO_H 1

#include <cstddef>
#include <cstdio>
#include <string>
#include <sys/types.h>

namespace madc {
namespace detail {

bool set_fd_close_on_exec(int fd);
ssize_t write_fd_without_sigpipe(int fd, const void *buffer, std::size_t size);

// Canonical absolute form of `path` (POSIX realpath: resolves symlinks,
// `.` and `..`); empty string when it does not resolve. The Win32 arm
// (GetFullPathName) normalizes without following links — consumers use
// this for CONSISTENT path spellings, never for link identity.
std::string resolve_real_path(const char *path);

// This host's name (POSIX gethostname); empty string on failure. The
// Win32 arm rides GetComputerNameEx, NOT winsock's gethostname — callers
// run at static-init time, before any WSAStartup.
std::string get_host_name();

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

} // namespace detail
} // namespace madc

#endif // __MADC_POSIX_IO_H
