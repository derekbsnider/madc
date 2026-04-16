// madc embedded sys/select.h — select() multiplexing with fd_set support
// Functions (select, pselect) available via dlsym fallback.
// FD_SETSIZE is 1024; fd_set holds 1024 bits in 16 int64 slots (128 bytes),
// matching glibc x86-64 layout exactly. The FD_* macros forward to built-in
// C helpers (__madc_fd_zero/set/clr/isset) bundled into the madc binary.

#define FD_SETSIZE 1024

// 128-byte bit array laid out as 16 int64_t slots. Field names are internal
// and not intended for direct access — use the FD_* macros.
struct fd_set {
    int64_t __b0;
    int64_t __b1;
    int64_t __b2;
    int64_t __b3;
    int64_t __b4;
    int64_t __b5;
    int64_t __b6;
    int64_t __b7;
    int64_t __b8;
    int64_t __b9;
    int64_t __b10;
    int64_t __b11;
    int64_t __b12;
    int64_t __b13;
    int64_t __b14;
    int64_t __b15;
};

#define FD_ZERO(set)      __madc_fd_zero(&(set))
#define FD_SET(fd, set)   __madc_fd_set((fd), &(set))
#define FD_CLR(fd, set)   __madc_fd_clr((fd), &(set))
#define FD_ISSET(fd, set) __madc_fd_isset((fd), &(set))
