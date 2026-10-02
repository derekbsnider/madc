// madc embedded stdalign.h — C11 7.15. A compiler-provided freestanding
// header (gcc ships it in its resource dir, the slot the embedded set
// occupies). C only, and not from C23 on, where alignas and alignof are
// keywords (gcc 13's gating; clang 18 also defines the two
// __*_is_defined macros under C++).
#ifndef _STDALIGN_H
#define _STDALIGN_H
#if !defined __cplusplus && !(defined __STDC_VERSION__ && __STDC_VERSION__ > 201710L)
#define alignas _Alignas
#define alignof _Alignof
#define __alignas_is_defined 1
#define __alignof_is_defined 1
#endif
#endif
