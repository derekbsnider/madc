#ifndef __MADC_POSIX_IO_H
#define __MADC_POSIX_IO_H 1

#include <cstddef>
#include <sys/types.h>

namespace madc {
namespace detail {

ssize_t write_fd_without_sigpipe(int fd, const void *buffer, std::size_t size);

} // namespace detail
} // namespace madc

#endif // __MADC_POSIX_IO_H
