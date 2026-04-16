// madc embedded stdio.h — C standard I/O
// printf/fprintf/sprintf/snprintf are available via dlsym fallback (libc is always loaded)

#define EOF    -1
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 8192
#define NULL 0
