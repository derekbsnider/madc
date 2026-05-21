// madc embedded resolv.h — DNS resolver
// b64_ntop / b64_pton and resolver functions available via dlsym.

#ifndef __MADC_RESOLV_H
#define __MADC_RESOLV_H 1

#include <sys/types.h>

extern int b64_ntop(const unsigned char *src, int srclength,
                    char *target, int targsize);
extern int b64_pton(const char *src, unsigned char *target, int targsize);

#endif
