// madc embedded sys/time.h — POSIX time structures and constants
// Functions (gettimeofday, settimeofday, getitimer, setitimer) available
// via dlsym fallback. Use `(struct timeval *)` cast to access fields on
// the caller-allocated buffer.

// Interval timer types
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

// suseconds_t is signed long on glibc x86-64.
#define suseconds_t int64_t

// glibc x86-64 struct timeval — 16 bytes.
struct timeval {
    int64_t tv_sec;     // seconds
    int64_t tv_usec;    // microseconds
};

// struct timezone — deprecated by POSIX; glibc still accepts NULL for it.
struct timezone {
    int32_t tz_minuteswest;
    int32_t tz_dsttime;
};
