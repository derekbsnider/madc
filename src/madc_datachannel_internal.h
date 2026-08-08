#ifndef __MADC_DATACHANNEL_INTERNAL_H
#define __MADC_DATACHANNEL_INTERNAL_H 1

#include "libmadc/error.h"

#include <string>

namespace madc {

class DataChannelRegistry;

namespace detail {

void set_channel_error(error *err, const std::string &operation,
		       const std::string &detail);
void set_channel_errno(error *err, const std::string &operation,
		       const std::string &target);
void register_socket_channel_factories(DataChannelRegistry &registry);

} // namespace detail
} // namespace madc

#endif // __MADC_DATACHANNEL_INTERNAL_H
