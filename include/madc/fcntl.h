// madc embedded fcntl.h — file control constants (Linux x86-64 values)
// Functions (open, creat, fcntl) available via dlsym fallback

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    64
#define O_EXCL     128
#define O_NOCTTY   256
#define O_TRUNC    512
#define O_APPEND   1024
#define O_NONBLOCK 2048
#define O_DSYNC    4096
#define O_SYNC     1052672
#define O_CLOEXEC  524288
