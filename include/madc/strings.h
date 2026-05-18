// madc embedded strings.h — POSIX string functions
// Most functions resolve through the dlsym fallback.
// This stub satisfies #include <strings.h> for code that uses
// bcopy, bzero, strcasecmp, strncasecmp, etc.

#ifndef __MADC_STRINGS_H
#define __MADC_STRINGS_H 1

#include <string.h>

// POSIX strings.h functions — available via dlsym fallback
extern int strcasecmp(const char *s1, const char *s2);
extern int strncasecmp(const char *s1, const char *s2, int n);

#endif
