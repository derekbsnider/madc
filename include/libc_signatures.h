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

/* The class for one libc name, or LibcRet::Unknown when this table does not
 * know it. Handles the <math.h> families by suffix, so `log10f` and `log10l`
 * resolve without 171 explicit entries. */
LibcRet madc_libc_return_class(const std::string &name);

/* The C99 <math.h> function roots, NULL-terminated. THE list — the lexer
 * expands it into `__builtin_<root>{,f,l}` aliases and the table above expands
 * it into return classes. Two expansions of one list cannot disagree about
 * which names exist. */
const char *const *madc_libc_math_roots(void);

#endif /* __LIBC_SIGNATURES_H */
