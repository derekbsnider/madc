// madc embedded sys/select.h — select() multiplexing
// Functions (select, pselect) available via dlsym fallback
// struct fd_set manipulation macros (FD_SET, FD_CLR, FD_ISSET, FD_ZERO)
// are not implementable without function-like macros — use poll() instead

#define FD_SETSIZE 1024
