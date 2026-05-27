// madc embedded time.h — POSIX time constants, types, and struct tm.
// Functions (time, clock, difftime, mktime, localtime, gmtime, strftime,
// nanosleep) available via dlsym fallback. Use `(struct tm *)` cast on
// the return value of localtime/gmtime to attach field access.

// Clocks
#define CLOCKS_PER_SEC 1000000
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

// Time type aliases
#define time_t  int64_t
#define clock_t int64_t

// glibc x86-64 struct tm layout — 56 bytes total, natural C ABI alignment.
// All int fields are 32-bit (C int) to match glibc; tm_gmtoff is long (64-bit
// on LP64); tm_zone is const char *. The 4-byte pad between tm_isdst and
// tm_gmtoff is inserted automatically by madc's natural field alignment.
struct tm {
    int32_t tm_sec;     // seconds  [0, 60]
    int32_t tm_min;     // minutes  [0, 59]
    int32_t tm_hour;    // hours    [0, 23]
    int32_t tm_mday;    // day of month  [1, 31]
    int32_t tm_mon;     // months since Jan [0, 11]
    int32_t tm_year;    // years since 1900
    int32_t tm_wday;    // days since Sunday [0, 6]
    int32_t tm_yday;    // days since Jan 1  [0, 365]
    int32_t tm_isdst;   // DST flag
    int64_t tm_gmtoff;  // seconds east of UTC
    char   *tm_zone;    // timezone abbreviation
};

// Function prototypes (needed by transpiler — JIT uses dlsym fallback)
int64_t time(int64_t *tloc);
int64_t clock(void);
double difftime(int64_t time1, int64_t time0);
int64_t mktime(struct tm *timeptr);
struct tm *localtime(const int64_t *timer);
struct tm *gmtime(const int64_t *timer);
uint64_t strftime(char *s, uint64_t maxsize, const char *format, const struct tm *timeptr);
int nanosleep(const void *req, void *rem);
