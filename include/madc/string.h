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
