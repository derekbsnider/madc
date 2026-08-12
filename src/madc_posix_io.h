#ifndef __MADC_POSIX_IO_H
#define __MADC_POSIX_IO_H 1

#include <cstddef>
#include <sys/types.h>

namespace madc {
namespace detail {

bool set_fd_close_on_exec(int fd);
ssize_t write_fd_without_sigpipe(int fd, const void *buffer, std::size_t size);

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
