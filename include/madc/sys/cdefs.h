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

// Function/parameter attribute macros used throughout glibc headers. madc
// ignores GCC attributes, so — like glibc's own "compiler lacks attributes"
// fallback — these expand to nothing (or a passthrough). Without them a real
// prototype like `wchar_t *wcscpy(...) __THROW __nonnull((1,2));` leaves the
// trailing macro dangling after the parameter list and the parser reads it as
// the start of a K&R definition ("Expecting brace after function declaration").
#define __nonnull(params)
#define __attribute_nonnull__(params)
#define __attr_access(x)
#define __attr_access_none(argno)
#define __attr_dealloc(dealloc, argno)
#define __attr_dealloc_free
#define __wur
#define __attribute_malloc__
#define __attribute_pure__
#define __attribute_const__
#define __attribute_used__
#define __attribute_unused__
#define __attribute_noinline__
#define __attribute_deprecated__
#define __attribute_deprecated_msg__(msg)
#define __attribute_warn_unused_result__
#define __attribute_maybe_unused__
#define __attribute_returns_twice__
#define __attribute_format_arg__(x)
#define __attribute_format_strfmon__(a, b)
#define __glibc_macro_warning(msg)
#define __attribute_artificial__

// Inline-family macros (glibc's no-attribute fallback shapes). Used by the
// optimized inline definitions some libc headers ship under
// __USE_EXTERN_INLINES.
#define __always_inline __inline
#define __extern_inline extern __inline
#define __extern_always_inline extern __inline
#define __fortify_function extern __inline

#ifndef __inline
#define __inline inline
#endif

#endif
