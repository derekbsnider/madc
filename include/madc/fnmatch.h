// madc embedded fnmatch.h — filename pattern matching
// Functions (fnmatch) available via dlsym fallback

// fnmatch() flags
#define FNM_NOESCAPE    0x01
#define FNM_PATHNAME    0x02
#define FNM_PERIOD      0x04
#define FNM_FILE_NAME   0x02
#define FNM_LEADING_DIR 0x08
#define FNM_CASEFOLD    0x10
#define FNM_EXTMATCH    0x20

// fnmatch() return values
#define FNM_NOMATCH 1
