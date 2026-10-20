// madc embedded float.h — floating-point characteristics of the TARGET.
//
// gcc's own float.h model: every value is spelled through the predefined
// __FLT_*__ / __DBL_*__ / __LDBL_*__ macros the target compiler's table seeds
// (the stddef.h convention for its types), so one text serves the x87,
// binary128, binary64 and IBM double-double long doubles alike. The values it
// used to carry were x86-64's, baked in: LDBL_MANT_DIG read 64 on aarch64-linux
// (113) and Apple arm64 (53). The sections and their gates are gcc's: the
// C99 pair, the C11 additions, the C23 additions.

#ifndef __MADC_FLOAT_H
#define __MADC_FLOAT_H 1

#define FLT_RADIX	__FLT_RADIX__

#define FLT_MANT_DIG	__FLT_MANT_DIG__
#define DBL_MANT_DIG	__DBL_MANT_DIG__
#define LDBL_MANT_DIG	__LDBL_MANT_DIG__

#define FLT_DIG		__FLT_DIG__
#define DBL_DIG		__DBL_DIG__
#define LDBL_DIG	__LDBL_DIG__

#define FLT_MIN_EXP	__FLT_MIN_EXP__
#define DBL_MIN_EXP	__DBL_MIN_EXP__
#define LDBL_MIN_EXP	__LDBL_MIN_EXP__

#define FLT_MIN_10_EXP	__FLT_MIN_10_EXP__
#define DBL_MIN_10_EXP	__DBL_MIN_10_EXP__
#define LDBL_MIN_10_EXP	__LDBL_MIN_10_EXP__

#define FLT_MAX_EXP	__FLT_MAX_EXP__
#define DBL_MAX_EXP	__DBL_MAX_EXP__
#define LDBL_MAX_EXP	__LDBL_MAX_EXP__

#define FLT_MAX_10_EXP	__FLT_MAX_10_EXP__
#define DBL_MAX_10_EXP	__DBL_MAX_10_EXP__
#define LDBL_MAX_10_EXP	__LDBL_MAX_10_EXP__

#define FLT_MAX		__FLT_MAX__
#define DBL_MAX		__DBL_MAX__
#define LDBL_MAX	__LDBL_MAX__

#define FLT_EPSILON	__FLT_EPSILON__
#define DBL_EPSILON	__DBL_EPSILON__
#define LDBL_EPSILON	__LDBL_EPSILON__

#define FLT_MIN		__FLT_MIN__
#define DBL_MIN		__DBL_MIN__
#define LDBL_MIN	__LDBL_MIN__

#define FLT_ROUNDS 1

#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) \
    || (defined(__cplusplus) && __cplusplus >= 201103L)
#ifdef __STDC_WANT_IEC_60559_TYPES_EXT__
#define FLT_EVAL_METHOD	__FLT_EVAL_METHOD_TS_18661_3__
#else
#define FLT_EVAL_METHOD	__FLT_EVAL_METHOD__
#endif
#define DECIMAL_DIG	__DECIMAL_DIG__
#endif /* C99 */

#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) \
    || (defined(__cplusplus) && __cplusplus >= 201703L)
#define FLT_DECIMAL_DIG		__FLT_DECIMAL_DIG__
#define DBL_DECIMAL_DIG		__DBL_DECIMAL_DIG__
#define LDBL_DECIMAL_DIG	__LDBL_DECIMAL_DIG__

#define FLT_HAS_SUBNORM		__FLT_HAS_DENORM__
#define DBL_HAS_SUBNORM		__DBL_HAS_DENORM__
#define LDBL_HAS_SUBNORM	__LDBL_HAS_DENORM__

#define FLT_TRUE_MIN	__FLT_DENORM_MIN__
#define DBL_TRUE_MIN	__DBL_DENORM_MIN__
#define LDBL_TRUE_MIN	__LDBL_DENORM_MIN__
#endif /* C11 */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L
#define FLT_NORM_MAX	__FLT_NORM_MAX__
#define DBL_NORM_MAX	__DBL_NORM_MAX__
#define LDBL_NORM_MAX	__LDBL_NORM_MAX__

#define FLT_IS_IEC_60559	__FLT_IS_IEC_60559__
#define DBL_IS_IEC_60559	__DBL_IS_IEC_60559__
#define LDBL_IS_IEC_60559	__LDBL_IS_IEC_60559__

#if __FLT_HAS_INFINITY__
#define INFINITY	(__builtin_inff ())
#endif
#if __FLT_HAS_QUIET_NAN__
#define NAN		(__builtin_nanf (""))
#define FLT_SNAN	(__builtin_nansf (""))
#endif
#if __DBL_HAS_QUIET_NAN__
#define DBL_SNAN	(__builtin_nans (""))
#endif
#if __LDBL_HAS_QUIET_NAN__
#define LDBL_SNAN	(__builtin_nansl (""))
#endif
#endif /* C23 */

#endif
