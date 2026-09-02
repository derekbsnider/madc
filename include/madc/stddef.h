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

// Spelled through the toolchain-seeded __*_TYPE__ macros (gcc's own
// resource stddef.h model): the macros carry each TARGET's shape
// (unsigned long on LP64, unsigned long long on win64, unsigned short
// wchar_t there), so one text serves every lane — a hardcoded LP64
// spelling minted a 4-byte size_t on win64 and collided with the real
// header chain's typedef ("repeated declaration size_t").
#ifdef __need_size_t
typedef __SIZE_TYPE__ size_t;
#undef __need_size_t
#endif
#ifdef __need_ptrdiff_t
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#undef __need_ptrdiff_t
#endif
#ifdef __need_wchar_t
typedef __WCHAR_TYPE__ wchar_t;
#undef __need_wchar_t
#endif
#ifdef __need_NULL
// gcc's stddef.h spells this `#undef NULL` + an UNCONDITIONAL `#define`, with
// the comment "in case <stdio.h> has defined it" — a __need_NULL request must
// PRODUCE NULL, not merely leave it produced. An `#ifndef NULL` guard here
// diverges from that oracle and makes the definition conditional on whatever
// ran earlier in the translation unit.
//
// That divergence is invisible live and fatal frozen. The pack's canonical
// order puts <cstddef> first, so by the time glibc's <stdlib.h> issued its
// __need_NULL request the guard was already false: the freeze recorded
// stdlib.h's `define __need_NULL` and `undef __need_NULL` and NO `define NULL`,
// leaving a unit that cannot define NULL when bound on its own. A TU reaching
// stddef.h ONLY through the __need protocol then got no NULL at all
// (tests/smaug_requests_mud.mah, on the headerless lane).
#undef NULL
#define NULL ((void *)0)
#undef __need_NULL
#endif
#ifdef __need_wint_t
typedef __WINT_TYPE__ wint_t;
#undef __need_wint_t
#endif

#else /* the full header */

#ifndef __MADC_STDDEF_H
#define __MADC_STDDEF_H 1

// UNCONDITIONAL, like the __need arm and gcc's own stddef.h ("in case
// <stdio.h> has defined it"): an `#ifndef NULL` guard made this arm's
// definition depend on what ran earlier in the TU — invisible live, fatal
// frozen. The darwin pack's canonical order puts the flattened prelude
// (which defines NULL) before this unit, so the freeze recorded a stddef.h
// unit with NO NULL, and a TU whose only route to NULL is the bare-name
// auto-include (`c.next = NULL;`, zero includes) had none (darwin D4:
// testphpdumpptr, testprojectwiden). The pack's stddef.h is the freestanding
// header for the auto-include, so it must PRODUCE NULL, not merely leave it
// produced.
#undef NULL
#define NULL ((void *)0)

#define offsetof(type, member) ((__SIZE_TYPE__)&((type *)0)->member)

// Target-shaped via the seeded __*_TYPE__ macros — see the __need arm above.
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __SIZE_TYPE__ size_t;
typedef __WCHAR_TYPE__ wchar_t;

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
