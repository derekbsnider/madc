// madc embedded stdlib.h — standard library constants
// Most functions (free, exit, rand, srand, etc.) resolve through the dlsym
// fallback. The pointer-returning allocators below are DECLARED so the parser
// knows their real return type — see the block comment on the externs.

#ifndef NULL
#define NULL ((void *)0)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     2147483647

// Pointer-returning functions must be declared so the parser knows their real
// return type. Without a prototype they default to the dlsym-fallback long
// return, so `p = malloc(...)` / `s = getenv(...)` mismatch (the "assigning
// integer without cast to pointer" warning) — the same reasoning string.h
// applies to the str* family. `unsigned long` stands in for size_t (as the
// string.h copy family does) to avoid a size_t typedef dependency here.
extern void *malloc(unsigned long size);
extern void *calloc(unsigned long nmemb, unsigned long size);
extern void *realloc(void *ptr, unsigned long size);
extern char *getenv(char *name);
