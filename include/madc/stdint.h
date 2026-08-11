// madc embedded stdint.h — the resource-dir copy of C11 7.20, complete.
// int8_t..uint64_t, size_t, intptr_t are native madc types; everything else
// a conforming stdint.h declares is defined here IN TERMS of the exact-width
// natives, so the file is target-neutral by construction. This header sits
// at madc's compiler-resource-dir slot: standard-library wrappers
// (libc++/libstdc++ <cstdint> -> stdint.h) reach it by #include_next and
// `using ::int_least8_t` & co need REAL declarations — a stub that only
// carried the limit macros made every cstdint fail with "not a declaration
// in '::'".

#ifndef __MADC_STDINT_H
#define __MADC_STDINT_H 1

typedef int8_t  int_least8_t;   typedef uint8_t  uint_least8_t;
typedef int16_t int_least16_t;  typedef uint16_t uint_least16_t;
typedef int32_t int_least32_t;  typedef uint32_t uint_least32_t;
typedef int64_t int_least64_t;  typedef uint64_t uint_least64_t;

// Fast types: Darwin defines them as the exact-width types; the glibc model
// widens 16/32 to the register word. Sizes must match the platform ABI the
// program links against.
#if defined(__APPLE__)
typedef int8_t  int_fast8_t;    typedef uint8_t  uint_fast8_t;
typedef int16_t int_fast16_t;   typedef uint16_t uint_fast16_t;
typedef int32_t int_fast32_t;   typedef uint32_t uint_fast32_t;
typedef int64_t int_fast64_t;   typedef uint64_t uint_fast64_t;
#else
typedef int8_t  int_fast8_t;    typedef uint8_t  uint_fast8_t;
typedef long    int_fast16_t;   typedef unsigned long uint_fast16_t;
typedef long    int_fast32_t;   typedef unsigned long uint_fast32_t;
typedef long    int_fast64_t;   typedef unsigned long uint_fast64_t;
#endif

typedef long intmax_t;
typedef unsigned long uintmax_t;
typedef long int intptr_t;
typedef unsigned long int uintptr_t;

#define INT8_MIN    (-128)
#define INT8_MAX    127
#define UINT8_MAX   255
#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define UINT16_MAX  65535
#define INT32_MIN   (-2147483647-1)
#define INT32_MAX   2147483647
#define UINT32_MAX  4294967295U
#define INT64_MIN   (-9223372036854775807L-1)
#define INT64_MAX   9223372036854775807L
#define UINT64_MAX  18446744073709551615UL

#define INT_LEAST8_MIN    INT8_MIN
#define INT_LEAST8_MAX    INT8_MAX
#define UINT_LEAST8_MAX   UINT8_MAX
#define INT_LEAST16_MIN   INT16_MIN
#define INT_LEAST16_MAX   INT16_MAX
#define UINT_LEAST16_MAX  UINT16_MAX
#define INT_LEAST32_MIN   INT32_MIN
#define INT_LEAST32_MAX   INT32_MAX
#define UINT_LEAST32_MAX  UINT32_MAX
#define INT_LEAST64_MIN   INT64_MIN
#define INT_LEAST64_MAX   INT64_MAX
#define UINT_LEAST64_MAX  UINT64_MAX

#define INT_FAST8_MIN     INT8_MIN
#define INT_FAST8_MAX     INT8_MAX
#define UINT_FAST8_MAX    UINT8_MAX
#if defined(__APPLE__)
#define INT_FAST16_MIN    INT16_MIN
#define INT_FAST16_MAX    INT16_MAX
#define UINT_FAST16_MAX   UINT16_MAX
#define INT_FAST32_MIN    INT32_MIN
#define INT_FAST32_MAX    INT32_MAX
#define UINT_FAST32_MAX   UINT32_MAX
#else
#define INT_FAST16_MIN    INT64_MIN
#define INT_FAST16_MAX    INT64_MAX
#define UINT_FAST16_MAX   UINT64_MAX
#define INT_FAST32_MIN    INT64_MIN
#define INT_FAST32_MAX    INT64_MAX
#define UINT_FAST32_MAX   UINT64_MAX
#endif
#define INT_FAST64_MIN    INT64_MIN
#define INT_FAST64_MAX    INT64_MAX
#define UINT_FAST64_MAX   UINT64_MAX

#define INTPTR_MIN   INT64_MIN
#define INTPTR_MAX   INT64_MAX
#define UINTPTR_MAX  UINT64_MAX
#define INTMAX_MIN   INT64_MIN
#define INTMAX_MAX   INT64_MAX
#define UINTMAX_MAX  UINT64_MAX

#define PTRDIFF_MIN  INT64_MIN
#define PTRDIFF_MAX  INT64_MAX
#define SIZE_MAX     UINT64_MAX
#define SIG_ATOMIC_MIN INT32_MIN
#define SIG_ATOMIC_MAX INT32_MAX
#define WCHAR_MIN    INT32_MIN
#define WCHAR_MAX    INT32_MAX
#define WINT_MIN     0U
#define WINT_MAX     4294967295U

#define INT8_C(v)    v
#define INT16_C(v)   v
#define INT32_C(v)   v
#define INT64_C(v)   v ## L
#define UINT8_C(v)   v
#define UINT16_C(v)  v
#define UINT32_C(v)  v ## U
#define UINT64_C(v)  v ## UL
#define INTMAX_C(v)  v ## L
#define UINTMAX_C(v) v ## UL

#endif
