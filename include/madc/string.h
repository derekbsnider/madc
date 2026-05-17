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
