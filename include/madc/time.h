// madc embedded time.h — POSIX time constants and type aliases
// Functions (time, clock, difftime, mktime, localtime, gmtime, strftime, nanosleep)
// available via dlsym fallback
// struct tm access deferred (requires struct interop)

// Clocks
#define CLOCKS_PER_SEC 1000000
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

// Time type aliases
#define time_t  int64_t
#define clock_t int64_t
