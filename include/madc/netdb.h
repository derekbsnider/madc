// madc embedded netdb.h — network database constants
// Functions (getaddrinfo, freeaddrinfo, gai_strerror, getnameinfo,
//            gethostbyname, getservbyname, getservbyport, gethostname)
// available via dlsym fallback
// struct addrinfo / hostent access deferred

// getaddrinfo() flags
#define AI_PASSIVE      0x0001
#define AI_CANONNAME    0x0002
#define AI_NUMERICHOST  0x0004
#define AI_V4MAPPED     0x0008
#define AI_ALL          0x0010
#define AI_ADDRCONFIG   0x0020
#define AI_NUMERICSERV  0x0400

// getnameinfo() flags
#define NI_NUMERICHOST  1
#define NI_NUMERICSERV  2
#define NI_NOFQDN       4
#define NI_NAMEREQD     8
#define NI_DGRAM        16
#define NI_MAXHOST      1025
#define NI_MAXSERV      32

// getaddrinfo() error codes
#define EAI_BADFLAGS    -1
#define EAI_NONAME      -2
#define EAI_AGAIN       -3
#define EAI_FAIL        -4
#define EAI_FAMILY      -6
#define EAI_SOCKTYPE    -7
#define EAI_SERVICE     -8
#define EAI_MEMORY      -10
#define EAI_SYSTEM      -11
#define EAI_OVERFLOW    -12

// glibc x86-64 struct hostent — 32 bytes, natural C ABI alignment.
// Returned by gethostbyname() / gethostbyaddr().
struct hostent {
    char  *h_name;          // official name of host
    char **h_aliases;       // NULL-terminated alias list
    int    h_addrtype;      // host address type (AF_INET / AF_INET6)
    int    h_length;        // length of address (4 for v4, 16 for v6)
    char **h_addr_list;     // NULL-terminated array of pointers to addresses
};
#define h_addr h_addr_list[0]   // legacy single-address alias

// glibc x86-64 struct servent — 32 bytes, natural C ABI alignment.
// Returned by getservbyname() / getservbyport(). s_port holds the port
// in network byte order — use ntohs() to get a host-order integer.
// s_aliases is a NULL-terminated array of alternate service names.
// s_proto is typically "tcp" or "udp".
struct servent {
    char  *s_name;      // official service name
    char **s_aliases;   // NULL-terminated list of aliases
    int    s_port;      // port number (network byte order)
    char  *s_proto;     // protocol name
};

// Function prototypes (needed by transpiler — JIT uses dlsym fallback)
struct hostent *gethostbyname(const char *name);
struct hostent *gethostbyaddr(const void *addr, uint32_t len, int type);
struct servent *getservbyname(const char *name, const char *proto);
struct servent *getservbyport(int port, const char *proto);
int gethostname(char *name, uint64_t len);
