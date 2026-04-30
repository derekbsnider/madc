// madc embedded netinet/in.h — IP protocol constants and struct layouts.
// Functions (htons, htonl, ntohs, ntohl, inet_addr, inet_ntoa) available
// via dlsym fallback.

// IP protocols
#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_IPV6    41
#define IPPROTO_RAW     255

// Special IPv4 addresses (network byte order — use htonl() at runtime)
#define INADDR_ANY       0x00000000
#define INADDR_BROADCAST 0xffffffff
#define INADDR_LOOPBACK  0x7f000001
#define INADDR_NONE      0xffffffff

// IPv6 address length
#define IN6ADDR_ANY_INIT 0

// Port range
#define IPPORT_RESERVED 1024

// TCP socket options (for setsockopt with IPPROTO_TCP)
#define TCP_NODELAY     1
#define TCP_MAXSEG      2
#define TCP_KEEPIDLE    4
#define TCP_KEEPINTVL   5
#define TCP_KEEPCNT     6

// POSIX type aliases for network-order field widths.
#define sa_family_t uint16_t
#define in_port_t   uint16_t
#define in_addr_t   uint32_t
#define socklen_t   uint32_t

// glibc x86-64 struct in_addr — 4 bytes: a single uint32 in network order.
struct in_addr {
    uint32_t s_addr;
};

// glibc x86-64 struct sockaddr_in — 16 bytes, natural C ABI alignment.
// sin_addr occupies 4 bytes starting at offset 4; sin_zero is the 8-byte
// padding that makes sockaddr_in and sockaddr (BSD base) the same size for
// the traditional bind()/connect() cast trick.
struct sockaddr_in {
    uint16_t sin_family;    // AF_INET
    uint16_t sin_port;      // network byte order — use htons()
    struct in_addr sin_addr;
    int64_t  sin_zero;      // glibc declares as char[8]; same 8-byte padding
};
