// madc embedded dirent.h — directory entry constants and struct layout.
// Functions (opendir, readdir, closedir, rewinddir, telldir, seekdir)
// available via dlsym fallback. DIR * is an opaque glibc type; treat the
// opendir return value as a void * / int64 handle and pass it verbatim to
// readdir / closedir.

// `DIR` is the opaque directory-handle type returned by opendir(). glibc
// exposes it via typedef but the underlying struct is implementation-
// internal; madc code only holds pointers to it, so a zero-size struct
// alias is sufficient — `DIR *dp;` parses as `struct __dir_opaque *dp;`.
typedef struct __dir_opaque DIR;

// d_type values (linux-specific, not portable across Unix — fall back to
// stat() if a value is DT_UNKNOWN).
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

// Function prototypes (needed by transpiler — JIT uses dlsym fallback)
DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
void rewinddir(DIR *dirp);
long telldir(DIR *dirp);
void seekdir(DIR *dirp, long loc);

// glibc x86-64 struct dirent — 280 bytes total (275 active + 5 trailing
// pad to the 8-byte struct alignment). d_name is declared as char[256]
// matching NAME_MAX + 1; readdir guarantees a null terminator.
struct dirent {
    uint64_t d_ino;      // inode number
    int64_t  d_off;      // offset to next dirent (opaque — do not interpret)
    uint16_t d_reclen;   // length of this record
    uint8_t  d_type;     // DT_* file-type hint (DT_UNKNOWN if not provided)
    char     d_name[256]; // null-terminated filename
};
