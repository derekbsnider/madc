// madc embedded sys/select.h — select() multiplexing with fd_set support
// Functions (select, pselect) available via dlsym fallback.
// FD_SETSIZE is 1024; fd_set holds 1024 bits in 16 int64 slots (128 bytes),
// matching glibc x86-64 layout exactly. The FD_* macros forward to built-in
// C helpers (__madc_fd_zero/set/clr/isset) bundled into the madc binary.

#define FD_SETSIZE 1024

#ifndef __MADC_FD_SET_DEFINED
#define __MADC_FD_SET_DEFINED

// 128-byte bit array laid out as 16 int64_t slots. Field names are internal
// and not intended for direct access — use the FD_* macros.
struct fd_set {
    int64_t __b0;
    int64_t __b1;
    int64_t __b2;
    int64_t __b3;
    int64_t __b4;
    int64_t __b5;
    int64_t __b6;
    int64_t __b7;
    int64_t __b8;
    int64_t __b9;
    int64_t __b10;
    int64_t __b11;
    int64_t __b12;
    int64_t __b13;
    int64_t __b14;
    int64_t __b15;
};

// Bare `fd_set` (typedef alias) is the C-portable spelling used by
// most network code. Without it, `fd_set in_set;` fails with "use of
// undeclared identifier 'fd_set'".
typedef struct fd_set fd_set;

#endif

// FD_* macros take a `fd_set *` (a pointer), matching glibc. Callers
// pass either `&local_set` or an existing `fd_set *` parameter; both
// expand cleanly without a stray `&(&...)` doubling. Earlier versions
// of these macros baked `&(set)` into the body, which made the
// pointer form `FD_CLR(fd, &in_set)` fail with "expecting addressable
// expression after '&('".
#define FD_ZERO(setp)      __madc_fd_zero((setp))
#define FD_SET(fd, setp)   __madc_fd_set((fd), (setp))
#define FD_CLR(fd, setp)   __madc_fd_clr((fd), (setp))
#define FD_ISSET(fd, setp) __madc_fd_isset((fd), (setp))

// Function prototypes (needed by transpiler — JIT uses dlsym fallback)
int select(int nfds, struct fd_set *readfds, struct fd_set *writefds,
           struct fd_set *exceptfds, void *timeout);
