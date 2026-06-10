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

// fpos_t — real glibc layout (struct _G_fpos_t: __off_t __pos + __mbstate_t
// __state; 16 bytes, 8-aligned) so real <cstdio>'s `using ::fpos_t;` resolves
// and fgetpos/fsetpos interoperate with libc. fgetpos/fsetpos return signed
// int (0 / nonzero) — they must not fall to the 64-bit dlsym default.
typedef struct {
	long __pos;
	int __count;
	int __value;
} fpos_t;

extern int fgetpos(void *stream, fpos_t *pos);
extern int fsetpos(void *stream, fpos_t *pos);

// Real <cstdio> imports the WHOLE C89 stdio surface with `namespace std {
// using ::clearerr; ... }`, and a using-declaration needs a global-scope
// declaration to bind to (same situation and fix as ctype.h — see the
// embedded-headers rule). Signed-int returns are declared as int (NOT the
// 64-bit dlsym default); size_t returns as size_t; ftell as long. The
// printf/scanf family is declared with its real variadic prototype — the
// real-header path already parses these from glibc stdio.h.
extern void clearerr(void *stream);
extern int fclose(void *stream);
extern int feof(void *stream);
extern int ferror(void *stream);
extern int fflush(void *stream);
extern int fgetc(void *stream);
extern int fprintf(void *stream, char *fmt, ...);
extern int fputc(int c, void *stream);
extern int fputs(char *s, void *stream);
extern size_t fread(void *ptr, size_t size, size_t nmemb, void *stream);
extern int fscanf(void *stream, char *fmt, ...);
extern int fseek(void *stream, long offset, int whence);
extern long ftell(void *stream);
extern size_t fwrite(void *ptr, size_t size, size_t nmemb, void *stream);
extern int getc(void *stream);
extern int getchar(void);
extern void perror(char *s);
extern int printf(char *fmt, ...);
extern int putc(int c, void *stream);
extern int putchar(int c);
extern int puts(char *s);
extern int remove(char *pathname);
extern int rename(char *oldpath, char *newpath);
extern void rewind(void *stream);
extern int scanf(char *fmt, ...);
extern void setbuf(void *stream, char *buf);
extern int setvbuf(void *stream, char *buf, int mode, size_t size);
extern int sprintf(char *str, char *fmt, ...);
extern int snprintf(char *str, size_t size, char *fmt, ...);
extern int sscanf(char *str, char *fmt, ...);
extern int ungetc(int c, void *stream);
extern int vfprintf(void *stream, char *fmt, void *ap);
extern int vfscanf(void *stream, char *fmt, void *ap);
extern int vprintf(char *fmt, void *ap);
extern int vscanf(char *fmt, void *ap);
extern int vsnprintf(char *str, size_t size, char *fmt, void *ap);
extern int vsprintf(char *str, char *fmt, void *ap);
extern int vsscanf(char *str, char *fmt, void *ap);
