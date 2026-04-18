// madc embedded sys/stat.h — file-status constants and struct layout.
// Functions (stat, fstat, lstat, chmod, mkdir, mkfifo) available via dlsym
// fallback. Use `(struct stat *)` to attach field access to a caller-allocated
// buffer, or `struct stat sbuf; stat(path, &sbuf);` directly.

// File-type bits (for st_mode)
#define S_IFMT   0xF000
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define S_IFLNK  0xA000
#define S_IFBLK  0x6000
#define S_IFCHR  0x2000
#define S_IFIFO  0x1000
#define S_IFSOCK 0xC000

// Permission bits
#define S_ISUID  0x800
#define S_ISGID  0x400
#define S_ISVTX  0x200

#define S_IRUSR  0x100
#define S_IWUSR  0x80
#define S_IXUSR  0x40
#define S_IRWXU  0x1C0

#define S_IRGRP  0x20
#define S_IWGRP  0x10
#define S_IXGRP  0x8
#define S_IRWXG  0x38

#define S_IROTH  0x4
#define S_IWOTH  0x2
#define S_IXOTH  0x1
#define S_IRWXO  0x7

// File-type predicate macros (C-library style, evaluate st_mode).
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

// Type aliases (glibc x86-64 LP64).
#define mode_t    uint32_t
#define uid_t     uint32_t
#define gid_t     uint32_t
#define dev_t     uint64_t
#define ino_t     uint64_t
#define nlink_t   uint64_t
#define off_t     int64_t
#define blksize_t int64_t
#define blkcnt_t  int64_t

// struct timespec — 16 bytes, glibc x86-64 layout (shared with time.h).
struct timespec {
    int64_t tv_sec;      // seconds
    int64_t tv_nsec;     // nanoseconds
};

// glibc x86-64 struct stat — 144 bytes total, natural C ABI alignment.
// Layout matches <bits/stat.h>: nlink comes before mode, with a 4-byte __pad0
// between st_gid and st_rdev; timespec triplet at the tail followed by 3 x
// reserved int64 slots. madc's natural field alignment inserts the __pad0
// automatically (st_rdev aligns to 8 after three 4-byte fields).
struct stat {
    uint64_t st_dev;     // device ID
    uint64_t st_ino;     // inode number
    uint64_t st_nlink;   // hard-link count
    uint32_t st_mode;    // file-type + permissions
    uint32_t st_uid;     // owner user ID
    uint32_t st_gid;     // owner group ID
    int32_t  __pad0;     // explicit; natural alignment would pad anyway
    uint64_t st_rdev;    // device ID (if special file)
    int64_t  st_size;    // size in bytes
    int64_t  st_blksize; // preferred I/O block size
    int64_t  st_blocks;  // 512-byte blocks allocated
    struct timespec st_atim;  // last access
    struct timespec st_mtim;  // last modification
    struct timespec st_ctim;  // last status change
    int64_t  __glibc_reserved0;
    int64_t  __glibc_reserved1;
    int64_t  __glibc_reserved2;
};

// Legacy field aliases. glibc exposes these as macros pointing at the tv_sec
// of the corresponding timespec, so `sbuf.st_mtime` yields an int64 seconds.
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
