// madc embedded stdnoreturn.h — C11 7.23. A compiler-provided freestanding
// header (the resource-dir slot). C only: C++ spells it [[noreturn]].
#ifndef _STDNORETURN_H
#define _STDNORETURN_H
#ifndef __cplusplus
#define noreturn _Noreturn
#endif
#endif
