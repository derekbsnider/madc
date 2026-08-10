// madc embedded stddef.h — standard definitions
// size_t and ptrdiff_t are already native madc types via typedef
// This header provides NULL, offsetof, and max_align_t

// glibc's __need protocol FIRST, like every real resource-dir stddef.h
// (clang's and gcc's both implement it): a glibc header asking for one
// definition gets it, and the request macros are CLEARED. Leaving them
// defined re-routes libc++'s stddef.h wrapper away from its
// _LIBCPP_STDDEF_H arm on every later visit — <cstddef> #errors that its
// wrapper was bypassed. A __need visit must NOT set the full-content
// guard: the next plain include still owes the whole surface.
#if defined(__need_size_t) || defined(__need_ptrdiff_t) \
    || defined(__need_wchar_t) || defined(__need_NULL) \
    || defined(__need_wint_t)

#ifdef __need_size_t
typedef unsigned long size_t;
#undef __need_size_t
#endif
#ifdef __need_ptrdiff_t
typedef long ptrdiff_t;
#undef __need_ptrdiff_t
#endif
#ifdef __need_wchar_t
typedef int wchar_t;
#undef __need_wchar_t
#endif
#ifdef __need_NULL
#ifndef NULL
#define NULL ((void *)0)
#endif
#undef __need_NULL
#endif
#ifdef __need_wint_t
typedef unsigned int wint_t;
#undef __need_wint_t
#endif

#else /* the full header */

#ifndef __MADC_STDDEF_H
#define __MADC_STDDEF_H 1

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) ((unsigned long)&((type *)0)->member)

typedef long ptrdiff_t;
typedef unsigned long size_t;
typedef int wchar_t;

// C11 max_align_t. The members' natural alignments (long long, long double)
// give the platform's strictest fundamental alignment on both x86-64 (16,
// x87 long double) and arm64 (long double == double) without needing
// __attribute__((aligned)) — <cstddef>'s `using ::max_align_t` needs the
// declaration to exist. Guarded by the guard macros clang's and gcc's own
// stddef.h set when THEY define it: the hosted-darwin prelude is flattened
// clang output whose kept #defines include __CLANG_MAX_ALIGN_T_DEFINED, and
// an anonymous-struct typedef can never pass the repeated-identical-typedef
// dedup. (Prelude-first ordering is the packed groves' canonical order; an
// embedded-first live parse would still collide with the prelude's
// unconditional flattened copy — flattened text cannot re-gain its guard.)
#if !defined(__CLANG_MAX_ALIGN_T_DEFINED) && !defined(_GCC_MAX_ALIGN_T)
typedef struct {
	long long __madc_max_align_ll;
	long double __madc_max_align_ld;
} max_align_t;
#define __CLANG_MAX_ALIGN_T_DEFINED 1
#define _GCC_MAX_ALIGN_T 1
#endif

#endif /* __MADC_STDDEF_H */

#endif /* __need protocol */
