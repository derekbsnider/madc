// madc embedded fcntl.h — file control constants (Linux x86-64 values)
// Functions (open, creat, fcntl) available via dlsym fallback

#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_CREAT      64
#define O_EXCL       128
#define O_NOCTTY     256
#define O_TRUNC      512
#define O_APPEND     1024
#define O_NONBLOCK   2048
#define O_NDELAY     2048
#define O_DSYNC      4096
#define O_ASYNC      8192
#define O_DIRECTORY  65536
#define O_NOFOLLOW   131072
#define O_SYNC       1052672
#define O_CLOEXEC    524288

#define F_DUPFD           0
#define F_GETFD           1
#define F_SETFD           2
#define F_GETFL           3
#define F_SETFL           4
#define F_GETLK           5
#define F_SETLK           6
#define F_SETLKW          7
#define F_SETOWN          8
#define F_GETOWN          9
#define F_DUPFD_CLOEXEC   1030

#define FD_CLOEXEC   1
