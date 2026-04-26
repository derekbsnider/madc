// madc embedded sys/ioctl.h — minimal stubs for IMC sources.
// ioctl() resolves via dlsym. Common request codes that IMC uses
// (TIOCINQ, TIOCOUTQ, FIONREAD) match glibc x86-64 values.

#define FIONREAD 0x541B
#define TIOCINQ  FIONREAD
#define TIOCOUTQ 0x5411
