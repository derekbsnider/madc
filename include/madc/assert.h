// madc embedded assert.h — runtime assertions

#ifndef __MADC_ASSERT_H
#define __MADC_ASSERT_H 1

#load "libc.so.6" as __libc;

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) ((expr) ? (void)0 : abort())
#endif

#endif
