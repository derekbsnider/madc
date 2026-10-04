#ifndef __MADC_DATACHANNEL_INTERNAL_H
#define __MADC_DATACHANNEL_INTERNAL_H 1

#include "madcdis/datachannel.h"
#include "libmadc/error.h"

#include <memory>
#include <string>

namespace madc {

class DataChannelRegistry;

namespace detail {

// The one composition owner for runtime-phase errors in the channel/process
// subsystem; sibling helpers delegate here (check-one-error-composer.sh).
void set_channel_error(error *err, const std::string &message);
void set_channel_error(error *err, const std::string &operation,
		       const std::string &detail);
void set_channel_errno(error *err, const std::string &operation,
		       const std::string &target);
void register_socket_channel_factories(DataChannelRegistry &registry);
// A connected stream socket the caller already holds (one end of a
// socketpair) as a byte-stream channel that owns it from here on: a read of 0
// bytes is the peer's end, a write never raises SIGPIPE. `name` is its scheme
// and endpoint in errors.
std::unique_ptr<DataChannel> socket_channel_over(int fd, const std::string &name);
void register_exec_channel_factory(DataChannelRegistry &registry);
// The one owner of "open a filesystem path as a DataChannel" — the file/pipe
// scheme factory and the record-file storage drivers all delegate here.
std::unique_ptr<DataChannel> open_file_channel(const std::string &path,
					       ChannelOpenMode mode,
					       error *err);

} // namespace detail
} // namespace madc

#endif // __MADC_DATACHANNEL_INTERNAL_H
