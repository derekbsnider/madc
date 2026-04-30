// madc embedded sys/socket.h — POSIX socket constants and base struct.
// Functions (socket, bind, connect, listen, accept, send, recv,
//            sendto, recvfrom, setsockopt, getsockopt, shutdown,
//            getpeername, getsockname) available via dlsym fallback.
// struct sockaddr is the 16-byte generic base used to cast specific
// sockaddr_in / sockaddr_in6 / sockaddr_un / etc. for bind()/connect().
// sa_data is declared as two 56-bit opaque chunks here; the concrete
// family-specific layout lives in netinet/in.h and friends.

struct sockaddr {
    uint16_t sa_family;  // AF_* (AF_INET, AF_INET6, ...)
    int16_t  __sa_pad0;
    int32_t  __sa_pad1;
    int64_t  __sa_pad2;  // 16 bytes total, matches glibc sockaddr
};

// Address families
#define AF_UNSPEC  0
#define AF_UNIX    1
#define AF_LOCAL   1
#define AF_INET    2
#define AF_INET6   10
#define AF_PACKET  17

// Protocol families (aliases for AF_*)
#define PF_UNSPEC  0
#define PF_UNIX    1
#define PF_LOCAL   1
#define PF_INET    2
#define PF_INET6   10

// Socket types
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK  2048
#define SOCK_CLOEXEC   524288

// Socket-level option (for setsockopt/getsockopt level param)
#define SOL_SOCKET 1

// Socket options (SO_*)
#define SO_DEBUG        1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_DONTROUTE    5
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_OOBINLINE    10
#define SO_LINGER       13
#define SO_RCVLOWAT     18
#define SO_SNDLOWAT     19
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_REUSEPORT    15
#define SO_PASSCRED     16
#define SO_PEERCRED     17

// Shutdown how values
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

// Send/recv flags
#define MSG_OOB        1
#define MSG_PEEK       2
#define MSG_DONTROUTE  4
#define MSG_CTRUNC     8
#define MSG_PROXY      16
#define MSG_TRUNC      32
#define MSG_DONTWAIT   64
#define MSG_EOR        128
#define MSG_WAITALL    256
#define MSG_NOSIGNAL   16384
#define MSG_MORE       32768
