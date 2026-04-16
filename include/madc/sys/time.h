// madc embedded sys/time.h — POSIX time structures and constants
// Functions (gettimeofday, settimeofday, getitimer, setitimer)
// available via dlsym fallback
// struct timeval / struct timezone access deferred

// Interval timer types
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2
