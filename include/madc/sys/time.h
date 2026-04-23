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

#define FD_ZERO(set)      __madc_fd_zero(&(set))
#define FD_SET(fd, set)   __madc_fd_set((fd), &(set))
#define FD_CLR(fd, set)   __madc_fd_clr((fd), &(set))
#define FD_ISSET(fd, set) __madc_fd_isset((fd), &(set))

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
