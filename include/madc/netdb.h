// madc embedded netdb.h — network database constants
// Functions (getaddrinfo, freeaddrinfo, gai_strerror, getnameinfo,
//            gethostbyname, getservbyname, getservbyport, gethostname)
// available via dlsym fallback
// struct addrinfo / hostent / servent access deferred

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
