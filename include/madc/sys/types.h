// madc embedded sys/types.h — POSIX type aliases
// These supplement the aliases in unistd.h

#define pid_t    int
#define uid_t    int
#define gid_t    int
#define off_t    int64_t
#define off64_t  int64_t
#define size_t   uint64_t
#define ssize_t  int64_t
#define mode_t   int
#define dev_t    int64_t
#define ino_t    uint64_t
#define nlink_t  int64_t
#define blksize_t int64_t
#define blkcnt_t  int64_t
#define socklen_t int
#define sa_family_t int
