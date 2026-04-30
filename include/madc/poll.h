// madc embedded poll.h — poll() I/O multiplexing
// Functions (poll, ppoll) available via dlsym fallback
// struct pollfd access deferred (requires struct interop)

// Events to poll for (events / revents bitmask)
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
#define POLLRDHUP 0x2000
