// madc embedded string.h — C string functions
// Most functions resolve through the dlsym fallback at parse time
// (which registers them with a generic int64 return signature). The
// extern declarations below give the parser proper return types so
// `*(strchr(...)) = 0` works without explicit user-side `extern`.

#ifndef NULL
#define NULL ((void *)0)
#endif

extern char *strchr(char *s, int c);
extern char *strrchr(char *s, int c);
extern char *strstr(char *haystack, char *needle);
extern char *strdup(char *s);
extern char *strpbrk(char *s, char *accept);
extern char *strtok(char *s, char *delim);
extern char *strndup(char *s, int n);
// The copy/concat family also return char* (the destination). Without these
// declarations they default to a long return, so e.g.
// `cond ? one_argument(...) : strcpy(...)` mismatches char* vs long.
extern char *strcpy(char *dest, char *src);
extern char *strncpy(char *dest, char *src, unsigned long n);
extern char *strcat(char *dest, char *src);
extern char *strncat(char *dest, char *src, unsigned long n);
// The comparison family returns int, NOT the dlsym-fallback default of long.
// This matters: their result is negative for "less than", and that negative
// is routinely used directly in a signed comparison (e.g. SMAUG's
// `bsearch_skill_exact`: `strcmp(name, ...) < 1`). With a long return, libc
// leaves only the low 32 bits set (the value is returned in eax), so a
// negative int read as a 64-bit long becomes a huge positive — every such
// comparison goes the wrong way (the binary search always returned -1, so
// skill_lookup failed and combat dereferenced skill_table[bad gsn]).
extern int strcmp(char *a, char *b);
extern int strncmp(char *a, char *b, unsigned long n);
extern int strcasecmp(char *a, char *b);
extern int strncasecmp(char *a, char *b, unsigned long n);
extern int memcmp(void *a, void *b, unsigned long n);
