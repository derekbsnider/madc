// madc embedded sys/mman.h — memory mapping constants (Linux x86-64)
// Functions (mmap, munmap, mprotect, msync, madvise, mlockall, munlockall)
// available via dlsym fallback
// Note: mmap returns void* (use int64_t to hold the address)

// Memory protection flags (prot param to mmap/mprotect)
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

// Mapping type flags (flags param to mmap)
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_ANON       0x20
#define MAP_GROWSDOWN  0x0100
#define MAP_DENYWRITE  0x0800
#define MAP_EXECUTABLE 0x1000
#define MAP_LOCKED     0x2000
#define MAP_NORESERVE  0x4000
#define MAP_POPULATE   0x8000
#define MAP_NONBLOCK   0x10000
#define MAP_HUGETLB    0x40000

// mmap failure return value
#define MAP_FAILED -1

// msync flags
#define MS_ASYNC      1
#define MS_SYNC       4
#define MS_INVALIDATE 2

// madvise flags
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8
