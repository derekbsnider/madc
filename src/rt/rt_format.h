#ifndef __RT_FORMAT_H
#define __RT_FORMAT_H 1

/* rt_format.h — the std::format / std::print / std::println engine contract.
 *
 * ONE implementation of the std format-string grammar and the format-spec
 * mini-language ([format.string.std]), used from BOTH sides of the compiler
 * boundary, which is why it lives in the strict-C11 ledger lane:
 *
 *   src/cir_builder.cpp   parses/validates the LITERAL format string at
 *                         COMPILE time (calls the iterator + spec parser
 *                         below, linked into madc itself) and lowers each
 *                         call site to the typed primitives;
 *   src/rt/rt_format.c    emits the bytes at RUN time — and is the future
 *                         home of std::vformat, which runs the SAME iterator
 *                         over a runtime string (no second grammar).
 *
 * The oracle is libstdc++ std::format (g++ -std=c++23): every behavior here
 * is pinned by tests/unit/rt_format_oracle.inc, generated from the real
 * library by scripts/gen_format_oracle.cpp. Notable pinned rules:
 *   - default float ({}) is the SHORTEST ROUND-TRIP form; fixed vs scientific
 *     by rendered length, fixed winning ties ("10000", "0.001", "1e+05");
 *   - 'a'/'A' are printf %a/%A with the 0x/0X prefix stripped;
 *   - char strings are BYTE strings: width counts bytes, precision truncates
 *     bytes (libstdc++ 13 splits a UTF-8 sequence mid-byte; so do we);
 *   - zero-pad is ignored for inf/nan and whenever an explicit align is given;
 *   - negative integers in b/o/x/X render sign-then-prefix ("-0x2a").
 *
 * Output goes through the dump runtime's byte sink (rt_dump.h): NULL sink ->
 * stdout, non-NULL -> the growable capture buffer. std::format captures into
 * a sink and hands the bytes to std::string host-side; std::print writes the
 * NULL sink. Plain C, included by rt_format.c itself so these prototypes are
 * CHECKED against the definitions.
 */

#include <stddef.h>		/* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* One parsed format-spec ([[fill]align][sign][#][0][width][.prec][type]). */
typedef struct madc_fmt_spec {
	unsigned char fill;	/* fill BYTE (ASCII; default ' ') */
	char align;		/* '<' '>' '^' or 0 = the type's default */
	char sign;		/* '+' or ' '; 0 covers both absent and '-' */
	unsigned char alt;	/* '#' */
	unsigned char zero;	/* '0' pad (ignored when align is given) */
	int width;		/* -1 = none */
	int precision;		/* -1 = none */
	char type;		/* presentation char or 0 */
} madc_fmt_spec;

/* Parse the bytes AFTER the ':' of one replacement field. Returns NULL on
 * success, else a static English message — the compile-time caller turns it
 * into the diagnostic, so invalid specs never reach a primitive. Validity of
 * `type` FOR a given argument kind is the caller's check (it knows the kind);
 * this parses the shape only. */
const char *__madc_fmt_parse_spec(const char *spec, long long n,
				  madc_fmt_spec *out);

/* --- format-string iteration -------------------------------------------- */

enum {
	MADC_FMT_TEXT  = 0,	/* a literal run ("{{"/"}}" arrive as a
				 * one-byte run holding the brace) */
	MADC_FMT_FIELD = 1	/* one replacement field */
};

typedef struct madc_fmt_item {
	int kind;
	const char *text;	/* TEXT: run start (points into fmt) */
	long long text_n;
	int arg_id;		/* FIELD: explicit index, or -1 = automatic */
	const char *spec;	/* FIELD: bytes after ':' (may be empty) */
	long long spec_n;
} madc_fmt_item;

/* Step once from byte offset `pos`. Returns the offset AFTER the item (always
 * > pos), or -1 when pos == n (done), or -2 on a malformed string with *err
 * set to a static message. Automatic-vs-manual indexing consistency is the
 * caller's check (it sees every field). */
long long __madc_fmt_next(const char *fmt, long long n, long long pos,
			  madc_fmt_item *out, const char **err);

/* --- emission primitives -------------------------------------------------- */
/* Each takes the RAW spec bytes and parses them (one grammar owner; a spec
 * reaching here already passed compile-time validation — a runtime parse
 * failure emits a loud "[madc format failed: ...]" marker, never silence). */

void __madc_fmt_text(void *sink, const char *p, long long n);
void __madc_fmt_i64(void *sink, const char *spec, long long spec_n,
		    long long v);
void __madc_fmt_u64(void *sink, const char *spec, long long spec_n,
		    unsigned long long v);
void __madc_fmt_f64(void *sink, const char *spec, long long spec_n, double v);
void __madc_fmt_str_n(void *sink, const char *spec, long long spec_n,
		      const char *s, long long sn);
void __madc_fmt_cstr(void *sink, const char *spec, long long spec_n,
		     const char *s);	/* NUL-terminated convenience */
void __madc_fmt_char(void *sink, const char *spec, long long spec_n, int c);
void __madc_fmt_bool(void *sink, const char *spec, long long spec_n, int v);
void __madc_fmt_ptr(void *sink, const char *spec, long long spec_n,
		    const void *p);

#ifdef __cplusplus
}
#endif

#endif /* __RT_FORMAT_H */
