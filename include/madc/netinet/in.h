// madc embedded netinet/in.h — IP protocol constants (Linux x86-64)
// Functions (htons, htonl, ntohs, ntohl) available via dlsym fallback
// struct sockaddr_in / sockaddr_in6 / in_addr access deferred

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
