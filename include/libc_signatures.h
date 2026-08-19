#ifndef __LIBC_SIGNATURES_H
#define __LIBC_SIGNATURES_H 1

/* libc_signatures.h — what the C library's functions return.
 *
 * A compiler is supposed to know this. gcc keeps it in builtins.def; madc keeps
 * it here, because two front-end consumers need the same knowledge and a second
 * copy of it is the divergence this codebase gates against:
 *
 *   src/lexer.cpp   builds the `__builtin_X` -> `X` alias map, and needs the
 *                   C99 <math.h> root list to expand the *f / *l families.
 *   src/parser.cpp  types an UNDECLARED call that dlsym resolves, and needs
 *                   each of those same names' real return type.
 *
 * The second consumer is why this file exists. An undeclared `strcmp` used to be
 * declared `long long strcmp()`, so `strcmp(a,b) < 0` read a negative `int` out
 * of all of `rax` and evaluated FALSE; an undeclared `floor` read `xmm0` out of
 * `rax` and returned garbage. Both are legal C89 source and both exited 0 with
 * the wrong answer. Six math roots had been registered by hand, one bug report
 * at a time, from a list of fifty-seven — the seventh copy of the list is what
 * this file replaces.
 *
 * It covers TWO name families, because both arrive by the same route — an alias
 * target with no declaration in scope: the C library itself, and madc's own
 * runtime helpers that the alias map rewrites to (`__madc_bswap64`,
 * `__madc_builtin_memcpy_chk`, ...). Keeping both here is what lets one gate
 * assert that EVERY alias target has a known return type.
 *
 * DELIBERATELY FREE of DataDef / typespec_t: the caller maps a class onto its
 * own type objects (only the parser knows ddCHAR, and only it knows whether the
 * target's `long` is 4 or 8 bytes). This file states C's facts; it does not
 * model madc's types.
 *
 * The default is NOT in this table. An unlisted name returns `int`, which is
 * what C says an implicit declaration returns — see LibcRet::Unknown.
 */

#include <stdint.h>
#include <string>

/* The return-type classes the C library actually uses, as CLASSES rather than
 * widths: `long` is 4 bytes on LLP64 and 8 on LP64, and only the consumer knows
 * which target it is compiling for. Int64/UInt64 are the always-64 classes
 * (`long long`, `size_t`, `time_t`), which is why they are separate from
 * Long/ULong rather than merged with them. */
enum class LibcRet : uint8_t {
	Unknown,	/* not a name this table knows -> C's implicit `int` */
	Int,		/* explicitly int: listed only for the alias gate */
	Void,
	Long,		/* platform long   (4 on LLP64, 8 on LP64) */
	ULong,		/* platform unsigned long */
	Int64,		/* long long, time_t, off_t */
	UInt64,		/* unsigned long long, size_t, uintmax_t */
	UInt16,		/* unsigned short */
	UInt32,		/* unsigned int */
	Double,
	Float,
	LDouble,
	CharPtr,	/* char * */
	VoidPtr		/* void *, FILE *, DIR *, struct tm *, a function pointer */
};

/* The ARGUMENT shape of a <math.h> family member. A return class alone is not
 * enough for these: the fallback declares ZERO parameters — the variadic
 * convention — so C's default argument promotion turns a `float` into a `double`
 * and the real `floorf` reads a float out of half of it. floorf(3.9f) was 2.000
 * and fmodf(7,4) was -nan. `sqrtf` is the tell: it works only because it is one
 * of the six roots that were registered by hand, WITH a declared float parameter.
 *
 * `T` is the family's own real type — `double` for the bare root, `float` for the
 * `f` suffix, `long double` for `l` — the same suffix rule the return class uses,
 * so the two cannot disagree about a family.
 *
 * MATH ONLY, deliberately. The variadic convention is harmless for the rest of
 * the C library: pointers and integers pass unchanged, `char` promotes to `int`
 * which is what every <ctype.h> function takes anyway. `float` promotion is the
 * one default promotion that changes the callee's view of the bits. */
enum class LibcArgs : uint8_t {
	None,		/* not a shape this table knows -> variadic convention */
	T,		/* (T)             — the 1-argument majority */
	T_T,		/* (T,T)           — atan2 pow fmod hypot ... */
	T_T_T,		/* (T,T,T)         — fma */
	T_Int,		/* (T,int)         — ldexp scalbn */
	T_Long,		/* (T,long)        — scalbln */
	T_Tptr,		/* (T,T*)          — modf */
	T_Intptr,	/* (T,int*)        — frexp */
	T_T_Intptr,	/* (T,T,int*)      — remquo */
	T_LDouble	/* (T,long double) — nexttoward */
};

/* The class for one libc name, or LibcRet::Unknown when this table does not
 * know it. Handles the <math.h> families by suffix, so `log10f` and `log10l`
 * resolve without 171 explicit entries. */
LibcRet madc_libc_return_class(const std::string &name);

/* The C99 <math.h> function roots, NULL-terminated. THE list — the lexer
 * expands it into `__builtin_<root>{,f,l}` aliases and the table above expands
 * it into return classes. Two expansions of one list cannot disagree about
 * which names exist. */
const char *const *madc_libc_math_roots(void);

/* `name`'s argument shape when it is a <math.h> family member, else
 * LibcArgs::None. `family` (when non-NULL) receives the T of the shape as
 * LibcRet::Double / Float / LDouble — chosen by the SUFFIX, independent of the
 * return class, because `lroundf` takes a float and returns a long. */
LibcArgs madc_libc_arg_shape(const std::string &name, LibcRet *family);

#endif /* __LIBC_SIGNATURES_H */
