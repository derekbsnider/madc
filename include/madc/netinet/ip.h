// madc embedded netinet/ip.h — minimal IP header stub
// Most user code that includes this just expects the protocol
// constants; the struct ip layout is only needed by raw-socket code.
// Pull in netinet/in.h for the protocol numbers.

#include <netinet/in.h>
