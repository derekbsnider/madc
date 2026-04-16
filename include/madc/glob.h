// madc embedded glob.h — filename expansion
// Functions (glob, globfree) available via dlsym fallback
// struct glob_t access deferred

// glob() flags
#define GLOB_ERR      0x0001
#define GLOB_MARK     0x0002
#define GLOB_NOSORT   0x0004
#define GLOB_DOOFFS   0x0008
#define GLOB_NOCHECK  0x0010
#define GLOB_APPEND   0x0020
#define GLOB_NOESCAPE 0x0040
#define GLOB_PERIOD   0x0080
#define GLOB_ALTDIRFUNC 0x0100
#define GLOB_BRACE    0x0200
#define GLOB_NOMAGIC  0x0400
#define GLOB_TILDE    0x0800
#define GLOB_ONLYDIR  0x1000
#define GLOB_TILDE_CHECK 0x2000

// glob() return values (success = 0)
#define GLOB_NOSPACE 1
#define GLOB_ABORTED 2
#define GLOB_NOMATCH 3
