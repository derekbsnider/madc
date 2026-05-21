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
