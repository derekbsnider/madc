// madc embedded unistd.h — POSIX constants and type aliases
// Functions (read, write, close, lseek, fork, execvp, pipe, dup2,
//           getcwd, chdir, unlink, rmdir, access, getpid, getuid, sleep)
// available via dlsym fallback

// Standard file descriptors
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// access() mode flags
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

// lseek() whence values (mirrors stdio.h SEEK_*)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// POSIX type aliases — substituted at tokenization time via #define
#define pid_t    int
#define uid_t    int
#define gid_t    int
#define off_t    int64_t
#define size_t   uint64_t
#define ssize_t  int64_t
#define mode_t   int
#define dev_t    int64_t
#define ino_t    uint64_t
#define nlink_t  int64_t
#define intptr_t  int64_t
#define uintptr_t uint64_t
