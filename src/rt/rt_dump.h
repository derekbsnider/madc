#ifndef __RT_DUMP_H
#define __RT_DUMP_H 1

/* rt_dump.h — the php::print_r / php::var_dump output contract.
 *
 * ONE header for the whole dump runtime: the flavor discriminator, the column
 * geometry, and every output primitive's prototype. It exists because the dump
 * is walked in TWO places — a type is known in two different ways:
 *
 *   src/cir_dump.cpp        GENERATES the walk for a type the COMPILER knows,
 *                           so every column is a compile-time constant.
 *   src/rt_dump_value.cpp   WALKS a madc::value at RUN time, because a value's
 *                           kind is only known then, so it computes columns as
 *                           it descends.
 *   src/rt/rt_dump.c        emits the bytes for both.
 *
 * The indentation rule is one rule either way, so it has one owner (below).
 * Two copies of `8 * depth` would be the divergence-by-duplication this
 * codebase gates against: a change reaching only the generated side would
 * misalign a value nested inside a struct.
 *
 * Plain C, included by rt_dump.c itself so the prototypes here are CHECKED
 * against the definitions there — an extern-"C" mismatch between the two would
 * otherwise link silently and corrupt arguments.
 *
 * Every column and every byte of format is captured from php-cli 8.3.6 with
 * cat -A; the reasoning for each sits beside the primitive in rt_dump.c.
 */

#include <stddef.h>		/* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Which of the two renderings. Crosses the generated/runtime boundary as a call
 * argument, so both sides must agree on the encoding.
 * CirBuilder::DumpFlavor stays the compiler-side enum and converts to this at
 * the one boundary (enum-over-strings: convert at the edge, once). */
enum madc_dump_flavor {
	MADC_DUMP_PRINT_R  = 0,
	MADC_DUMP_VAR_DUMP = 1
};

/* The column of an aggregate's own frame — print_r's "(" and ")" lines,
 * var_dump's head and tail. print_r steps 8 per level, var_dump 2. */
static inline int madc_dump_frame_col(int flavor, int depth)
{
	return flavor == MADC_DUMP_VAR_DUMP ? 2 * depth : 8 * depth;
}

/* The column of one ENTRY inside that frame: print_r's "[k] => " sits 4 in from
 * the frame, var_dump's "[k]=>" 2 — which puts a var_dump key line at the same
 * column as the value line below it, as PHP does. */
static inline int madc_dump_entry_col(int flavor, int depth)
{
	return madc_dump_frame_col(flavor, depth)
	     + (flavor == MADC_DUMP_VAR_DUMP ? 2 : 4);
}

/* --- the capture sink (print_r's $return) ------------------------------- */
/* NULL sink -> stdout; non-NULL -> an opaque growable buffer. */
void       *__madc_dump_sink_open(void);
const char *__madc_dump_sink_text(void *sink);
size_t      __madc_dump_sink_length(void *sink);
int         __madc_dump_sink_failed(void *sink);
void        __madc_dump_sink_close(void *sink);

/* --- flavor-neutral ----------------------------------------------------- */
void __madc_dump_raw(void *sink, const char *p, long long n);

/* --- print_r ----------------------------------------------------------- */
void __madc_dump_pr_i64(void *sink, long long v, int is_unsigned);
void __madc_dump_pr_f64(void *sink, double v);
void __madc_dump_pr_bool(void *sink, int v);
void __madc_dump_pr_char(void *sink, int c);
void __madc_dump_pr_cstr(void *sink, const char *s);
void __madc_dump_pr_cstr_n(void *sink, const char *s, long long n);
void __madc_dump_pr_head(void *sink, int col, const char *word);
void __madc_dump_pr_key(void *sink, int col, const char *key);
void __madc_dump_pr_key_idx(void *sink, int col, long long idx);
void __madc_dump_pr_nl(void *sink);
void __madc_dump_pr_tail(void *sink, int col, int blank);
void __madc_dump_pr_recursion(void *sink, const char *word);
void __madc_dump_pr_instance(void *sink, unsigned type_id);

/* --- var_dump ---------------------------------------------------------- */
void __madc_dump_vd_null(void *sink, int col);
void __madc_dump_vd_recursion(void *sink, int col);
void __madc_dump_vd_instance(void *sink, int col, long long size,
			     unsigned type_id);
void __madc_dump_vd_head(void *sink, int col, const char *word,
			 long long count);
void __madc_dump_vd_key(void *sink, int col, const char *key);
void __madc_dump_vd_key_idx(void *sink, int col, long long idx);
void __madc_dump_vd_tail(void *sink, int col);
void __madc_dump_vd_i64(void *sink, int col, const char *ty, long long v,
			int is_unsigned);
void __madc_dump_vd_f64(void *sink, int col, const char *ty, double v);
void __madc_dump_vd_bool(void *sink, int col, const char *ty, int v);
void __madc_dump_vd_char(void *sink, int col, const char *ty, int c);
void __madc_dump_vd_cstr(void *sink, int col, const char *ty, const char *s);
void __madc_dump_vd_cstr_n(void *sink, int col, const char *ty, const char *s,
			   long long n);
void __madc_dump_vd_text_open(void *sink, int col, const char *ty,
			      long long len);
void __madc_dump_vd_text_close(void *sink);

/* --- the C++ half: the madc::value walk (src/rt_dump_value.cpp) --------- */
/* NOT part of the strict-C11 ledger lane, and it cannot be: a value's `array`
 * and `object` kinds are backed by C++ containers, so a program that dumps one
 * already needs the C++ script runtime (madarray_*). Declared here so the one
 * dump contract stays in one place. `v` is a `const madc::value *`, opaque to
 * C. `nested` is nonzero when this value sits inside an enclosing aggregate —
 * print_r follows a nested block's ")" with a blank line. */
void __madc_dump_value(void *sink, const void *v, int flavor, int depth,
		       int nested);

#ifdef __cplusplus
}
#endif

#endif /* __RT_DUMP_H */
