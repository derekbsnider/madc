#ifndef __MADC_SYS_CDEFS_H
#define __MADC_SYS_CDEFS_H 1

/*
 * Minimal glibc-style cdefs shim for embedded-header consumers.
 * Extend this only as upstream code or embedded headers require more.
 */

#define __P(args) args
#define __PMT(args) args

#define __CONCAT(x, y) x ## y
#define __STRING(x) #x

#define __ptr_t void *

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

#define __THROW
#define __THROWNL
#define __NTH(fct) fct
#define __NTHNL(fct) fct
#define __LEAF
#define __LEAF_ATTR
#define __COLD

#ifndef __inline
#define __inline inline
#endif

#endif
