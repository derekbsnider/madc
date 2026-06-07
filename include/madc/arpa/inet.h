// madc embedded arpa/inet.h — IPv4/IPv6 address conversion
// Most functions (inet_addr, inet_aton, inet_pton, htons, htonl, ntohs,
// ntohl) resolve through the dlsym fallback. The char*-returning ones are
// declared so their result doesn't default to the fallback long return
// (e.g. `strcpy(buf, inet_ntoa(addr))` would otherwise pass a long to a
// char* parameter).

#include <netinet/in.h>

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

extern char *inet_ntoa(struct in_addr in);
extern char *inet_ntop(int af, void *src, char *dst, unsigned int size);
