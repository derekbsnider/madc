// madc embedded sys/time.h — POSIX time structures and constants
// Functions (gettimeofday, settimeofday, getitimer, setitimer) available
// via dlsym fallback. Use `(struct timeval *)` cast to access fields on
// the caller-allocated buffer.

// Interval timer types
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

// fd_set is commonly surfaced alongside <sys/time.h> on older Unix codepaths.
// Keep this in sync with embedded sys/select.h so code that only includes
// <sys/time.h> can still declare select() sets.
#define FD_SETSIZE 1024

#ifndef __MADC_FD_SET_DEFINED
#define __MADC_FD_SET_DEFINED

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
typedef struct fd_set fd_set;

#endif

// FD_* macros take a `fd_set *` (pointer), matching glibc — see
// sys/select.h for rationale.
#define FD_ZERO(setp)      __madc_fd_zero((setp))
#define FD_SET(fd, setp)   __madc_fd_set((fd), (setp))
#define FD_CLR(fd, setp)   __madc_fd_clr((fd), (setp))
#define FD_ISSET(fd, setp) __madc_fd_isset((fd), (setp))

// time_t is int64_t on glibc x86-64.
#define time_t int64_t

// suseconds_t is signed long on glibc x86-64.
#define suseconds_t int64_t

// glibc x86-64 struct timeval — 16 bytes.
struct timeval {
    int64_t tv_sec;     // seconds
    int64_t tv_usec;    // microseconds
};

#define timerisset __madc_timerisset
#define timerclear __madc_timerclear
#define timercmp(left_ptr, right_ptr, cmp_token) ((__madc_timeval_sec((left_ptr)) == __madc_timeval_sec((right_ptr))) ? (__madc_timeval_usec((left_ptr)) cmp_token __madc_timeval_usec((right_ptr))) : (__madc_timeval_sec((left_ptr)) cmp_token __madc_timeval_sec((right_ptr))))
#define timeradd __madc_timeradd
#define timersub __madc_timersub

// struct timezone — deprecated by POSIX; glibc still accepts NULL for it.
struct timezone {
    int32_t tz_minuteswest;
    int32_t tz_dsttime;
};

// Function prototypes (needed by transpiler — JIT uses dlsym fallback)
int gettimeofday(struct timeval *tv, void *tz);
int settimeofday(const struct timeval *tv, const void *tz);
