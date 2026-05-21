// madc embedded stddef.h — standard definitions
// size_t and ptrdiff_t are already native madc types via typedef
// This header provides NULL, offsetof, and max_align_t

#ifndef __MADC_STDDEF_H
#define __MADC_STDDEF_H 1

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) ((unsigned long)&((type *)0)->member)

typedef long ptrdiff_t;
typedef unsigned long size_t;
typedef int wchar_t;

#endif
