// madc embedded stdio.h — C standard I/O
// printf/fprintf/sprintf/snprintf are available via dlsym fallback (libc is always loaded)
// FILE is treated as an opaque pointer-like type in madc source.

#define EOF    -1
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 8192
#define NULL 0
#define FILE void
#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
#define size_t uint64_t
#endif

// Pointer-returning I/O functions must be declared so the parser knows their
// real return type (FILE* — i.e. void* — or char*). Without a prototype they
// default to the dlsym-fallback long return, so `fp = fopen(...)` /
// `fgets(...)` mismatch (the "assigning integer without cast to pointer"
// warning). printf/scanf-family stay on the dlsym path (they are variadic).
extern void *fopen(char *path, char *mode);
extern void *freopen(char *path, char *mode, void *stream);
extern void *fdopen(int fd, char *mode);
extern void *popen(char *command, char *type);
extern void *tmpfile(void);
extern char *fgets(char *s, int size, void *stream);
extern char *gets(char *s);
extern char *tmpnam(char *s);
