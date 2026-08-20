///////////////////////////////////////////////////////////////////////////
//                                                                       //
// php::print_r / php::var_dump output runtime.                          //
//                                                                       //
// A DUAL-BUILD source (forest-carriers S5): compiled into libmadc by the //
// host build AND into an AOT ledger module by madc itself at pack time,  //
// so a `-static-libmadc` binary that dumps carries these inside its own  //
// image (Mach-O has no dylib fallback). Strict C11, no compiler          //
// builtins, no C++ runtime dependency — that is the ledger's membership  //
// rule (scripts/ledger_sources.txt).                                    //
//                                                                       //
// These are the OUTPUT PRIMITIVES only. The walk over a type's structure //
// is GENERATED per type by the CIR builder (src/cir_dump.cpp): the       //
// compiler knows T, so nothing here needs a runtime type descriptor.     //
//                                                                       //
// PHP is the oracle for every byte of the format                        //
// (docs/plans/2026-08-17-php-print-r-var-dump-plan.md captures php-cli   //
// 8.3.6 verbatim). Where C truth and PHP presentation differ, print_r    //
// follows PHP and var_dump follows C — that split is the whole design.   //
//                                                                       //
// EVERY primitive takes a leading `void *sink` (plan §13.4):             //
//   sink == NULL -> write to stdout (PHP's print_r($x) / var_dump($x))   //
//   sink != NULL -> append to an opaque growable buffer, which           //
//                   print_r($x, true) returns as text.                  //
// The sink is OPAQUE on purpose: generated code passes a `void *` and    //
// never needs this struct's layout, so the two sides cannot disagree     //
// about it. One writer (sink_write) owns the stdout-or-buffer decision,  //
// so no primitive can route output differently from its siblings.        //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// Every prototype below is CHECKED against its definition here. The header is
// the one dump contract, shared with the generated walk and the C++ value walk.
#include "rt_dump.h"

// stdout, unbuffered-order-wise, is shared with the C++ iostreams the rest of
// the runtime prints through (std::cout is sync_with_stdio by default), so a
// script may interleave `cout <<` and php::print_r freely.

// ---------------------------------------------------------------------------
// The sink
// ---------------------------------------------------------------------------
// A capture buffer for print_r($x, true). Deliberately NOT open_memstream:
// that is POSIX, and the win64 lane has no such function.

struct madc_dump_sink {
    char  *buf;
    size_t len;
    size_t cap;
    int    oom;			/* a failed grow is remembered, never silent */
};

void *__madc_dump_sink_open(void)
{
    struct madc_dump_sink *s = (struct madc_dump_sink *)
				  malloc(sizeof (struct madc_dump_sink));
    if (!s)
	return NULL;
    s->buf = NULL;
    s->len = 0;
    s->cap = 0;
    s->oom = 0;
    return (void *)s;
}

// The captured text, always NUL-terminated and never NULL, so the caller needs
// no branch. An empty capture reads as "".
const char *__madc_dump_sink_text(void *sink)
{
    struct madc_dump_sink *s = (struct madc_dump_sink *)sink;
    if (!s || !s->buf)
	return "";
    return s->buf;
}

// The captured LENGTH. The buffer is binary-safe — a value of kind `bytes` may
// contain a NUL and __madc_dump_raw writes it — so a caller that must not stop
// at the first NUL reads the length rather than calling strlen on the text.
size_t __madc_dump_sink_length(void *sink)
{
    struct madc_dump_sink *s = (struct madc_dump_sink *)sink;
    return s ? s->len : 0;
}

// Nonzero when any append failed, so the caller can tell an empty dump from a
// lost one rather than returning a plausible "".
int __madc_dump_sink_failed(void *sink)
{
    struct madc_dump_sink *s = (struct madc_dump_sink *)sink;
    return s ? s->oom : 0;
}

void __madc_dump_sink_close(void *sink)
{
    struct madc_dump_sink *s = (struct madc_dump_sink *)sink;
    if (!s)
	return;
    free(s->buf);
    free(s);
}

// THE writer. Every byte this file emits goes through here.
static void sink_write(void *sink, const char *s, size_t n)
{
    struct madc_dump_sink *k = (struct madc_dump_sink *)sink;
    size_t need;
    char *nb;

    if (!s || !n)
	return;
    if (!k) {
	fwrite(s, 1, n, stdout);
	return;
    }
    if (k->oom)
	return;
    need = k->len + n + 1;
    if (need > k->cap) {
	size_t cap = k->cap ? k->cap : 128;
	while (cap < need)
	    cap *= 2;
	nb = (char *)realloc(k->buf, cap);
	if (!nb) {
	    k->oom = 1;
	    return;
	}
	k->buf = nb;
	k->cap = cap;
    }
    memcpy(k->buf + k->len, s, n);
    k->len += n;
    k->buf[k->len] = '\0';
}

static void sink_puts(void *sink, const char *s)
{
    if (s)
	sink_write(sink, s, strlen(s));
}

static void sink_putc(void *sink, char c)
{
    sink_write(sink, &c, 1);
}

// Formatted write for the SHORT, bounded pieces only — numbers, punctuation and
// compile-time type words. Arbitrary-length user text (a string's characters, a
// type word of unknown length) is written with sink_puts instead, so nothing
// here can truncate a value. 256 covers every format in this file: the longest
// is a 64-bit decimal inside a few literal bytes.
static void sink_printf(void *sink, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0)
	return;
    if ((size_t)n >= sizeof buf)
	n = (int)(sizeof buf - 1);
    sink_write(sink, buf, (size_t)n);
}

// EXACTLY n bytes, NUL included. The public face of sink_write, for a payload
// whose length is known and which may contain a NUL: a madc::value's `string`
// and `bytes` kinds both carry an explicit byte count, so neither may be
// written with a NUL-terminated primitive. Flavor-neutral — the framing around
// it differs, the bytes do not.
void __madc_dump_raw(void *sink, const char *p, long long n)
{
    if (p && n > 0)
	sink_write(sink, p, (size_t)n);
}

// ---------------------------------------------------------------------------
// The ancestor stack
// ---------------------------------------------------------------------------
// The path currently being printed, by (address, type). See rt_dump.h for why
// it is a stack and not a visited set, why the tag is needed, and what the
// return values mean.

// Storage class of the per-thread path. Real TLS in the host build; the ledger
// build defines it EMPTY (-DMADC_RT_TLS=, madc_cir_ledger_compile) because MIR
// has no thread-local storage, so a `-static-libmadc` image carries a
// PROCESS-GLOBAL path. Single-threaded programs are unaffected; the honest fix
// is TLS in MIR (a Tier-3 floor gap, .claude/rules/lowering-vs-raising.md).
// Same pattern, same reason, as src/rt/rt_except.c.
#ifndef MADC_RT_TLS
#define MADC_RT_TLS _Thread_local
#endif

struct madc_dump_anc {
    const void *p;
    unsigned    tag;
};

static MADC_RT_TLS struct madc_dump_anc *madc_anc_buf = NULL;
static MADC_RT_TLS size_t madc_anc_len = 0;
static MADC_RT_TLS size_t madc_anc_cap = 0;

int __madc_dump_anc_push(const void *p, unsigned tag)
{
    size_t i;
    struct madc_dump_anc *nb;
    size_t cap;

    for (i = 0; i < madc_anc_len; i++)
	if (madc_anc_buf[i].p == p && madc_anc_buf[i].tag == tag)
	    return 0;			/* a cycle */
    if (madc_anc_len == madc_anc_cap) {
	cap = madc_anc_cap ? madc_anc_cap * 2 : 32;
	nb = (struct madc_dump_anc *)
		 realloc(madc_anc_buf, cap * sizeof *nb);
	if (!nb)
	    return -1;			/* never reported as a cycle */
	madc_anc_buf = nb;
	madc_anc_cap = cap;
    }
    madc_anc_buf[madc_anc_len].p = p;
    madc_anc_buf[madc_anc_len].tag = tag;
    madc_anc_len++;
    return 1;
}

// Popped only by a caller whose push RETURNED 1 — the capacity is deliberately
// kept for the next dump, so a program that dumps in a loop allocates once.
void __madc_dump_anc_pop(void)
{
    if (madc_anc_len)
	madc_anc_len--;
}

// The dump could not continue. Flavor-neutral: a failure is not a value, so it
// has no type word and no framing, and it is deliberately visible rather than
// silent.
void __madc_dump_fail(void *sink, const char *what)
{
    sink_puts(sink, "[madc dump failed: ");
    sink_puts(sink, what ? what : "unknown error");
    sink_puts(sink, "]\n");
}

// ---------------------------------------------------------------------------
// print_r scalars
// ---------------------------------------------------------------------------

// print_r of an integer: the decimal digits, NO newline and no type — PHP's
// print_r(42) is exactly "42". Signedness comes from the compiler, which knows
// the real type; the value rides as 64 bits either way.
void __madc_dump_pr_i64(void *sink, long long v, int is_unsigned)
{
    if (is_unsigned)
	sink_printf(sink, "%llu", (unsigned long long)v);
    else
	sink_printf(sink, "%lld", v);
}

// print_r of a floating value: PHP's own double->string, which is `%.14G`
// (precision=14) PLUS a guaranteed decimal point in the mantissa of an
// exponent form — PHP prints 1.0E+25 where C's %G prints 1E+25.
void __madc_dump_pr_f64(void *sink, double v)
{
    char buf[64];
    char out[72];
    size_t i, o, n;
    int seen_dot = 0, seen_exp = 0;

    snprintf(buf, sizeof(buf), "%.14G", v);
    n = strlen(buf);
    for (i = 0; i < n; i++) {
	if (buf[i] == '.')
	    seen_dot = 1;
	else if (buf[i] == 'E')
	    seen_exp = 1;
    }
    if (!seen_exp || seen_dot) {
	sink_puts(sink, buf);
	return;
    }
    for (i = 0, o = 0; i < n && o + 3 < sizeof(out); i++) {
	if (buf[i] == 'E') {
	    out[o++] = '.';
	    out[o++] = '0';
	}
	out[o++] = buf[i];
    }
    out[o] = '\0';
    sink_puts(sink, out);
}

// print_r of a bool: PHP renders true as "1" and false as the EMPTY string.
void __madc_dump_pr_bool(void *sink, int v)
{
    if (v)
	sink_puts(sink, "1");
}

// print_r of a character: the byte itself, as a one-character string. PHP has
// no char type; a PHP developer handed chr(65) sees "A".
void __madc_dump_pr_char(void *sink, int c)
{
    sink_putc(sink, (char)(unsigned char)c);
}

// print_r of a C string: the text. A NULL pointer is PHP's null, which print_r
// renders as the empty string.
void __madc_dump_pr_cstr(void *sink, const char *s)
{
    if (s)
	sink_puts(sink, s);
}

// print_r of a character ARRAY: text, bounded by the array's extent. A C array
// need not be NUL-terminated, so %s could read past it; stop at the extent or
// at the first NUL, whichever comes first.
void __madc_dump_pr_cstr_n(void *sink, const char *s, long long n)
{
    long long i;
    if (!s)
	return;
    for (i = 0; i < n && s[i]; i++)
	sink_putc(sink, s[i]);
}

// ---------------------------------------------------------------------------
// Aggregates
// ---------------------------------------------------------------------------
// PHP's print_r frames an array or object as
//
//     Array$                     <- the type word, then a newline
//     ($                         <- at column `col`
//         [0] => 1$              <- entries at col + 4
//     )$                         <- back at col
//
// and a NESTED one puts its type word on the "=> " line, indents its "(" a
// further 8, and follows its ")" with a BLANK line:
//
//         [1] => Array$
//             ($
//                 [0] => 2$
//             )$
//     $
//
// So col is 8*depth and entries sit at 8*depth+4. Every column here arrives as
// a compile-time constant: the generated dumper is expanded per nesting level,
// so no runtime depth counter exists. Captured from php-cli 8.3.6 with cat -A
// (tmp/or_pr2.php) — do not "tidy" the blank line or the 8-space step.

static void dump_indent(void *sink, int col)
{
    int i;
    for (i = 0; i < col; i++)
	sink_putc(sink, ' ');
}

// The aggregate's opening: its type word ("Array", "Point Object"), then "(".
void __madc_dump_pr_head(void *sink, int col, const char *word)
{
    sink_puts(sink, word ? word : "");
    sink_putc(sink, '\n');
    dump_indent(sink, col);
    sink_puts(sink, "(\n");
}

// One entry's key: PHP spells a protected member "[name:protected]" and a
// private one "[name:Class:private]". The whole key text is a compile-time
// literal — the compiler knows the access and the declaring class.
void __madc_dump_pr_key(void *sink, int col, const char *key)
{
    __madc_dump_pr_key_open(sink, col);
    sink_puts(sink, key ? key : "");
    __madc_dump_pr_key_close(sink);
}

// A key whose TEXT is a RUNTIME value — a container's key_type. The walk renders
// the key itself between these two, with the same primitives every other value
// goes through. The bracket spelling lives here once, which is why the literal
// form above is written in terms of it: two copies of "] => " would be free to
// drift, and a map's key line would then not match a member's.
void __madc_dump_pr_key_open(void *sink, int col)
{
    dump_indent(sink, col);
    sink_putc(sink, '[');
}

void __madc_dump_pr_key_close(void *sink)
{
    sink_puts(sink, "] => ");
}

// An array element's key: the position.
void __madc_dump_pr_key_idx(void *sink, int col, long long idx)
{
    dump_indent(sink, col);
    sink_printf(sink, "[%lld] => ", idx);
}

// End of a SCALAR entry. A nested aggregate ends itself (its ")" line plus the
// blank line), so this must not be emitted for one.
void __madc_dump_pr_nl(void *sink)
{
    sink_putc(sink, '\n');
}

// The aggregate's close. `blank` adds PHP's trailing empty line, which every
// NESTED block gets and the outermost one does not.
void __madc_dump_pr_tail(void *sink, int col, int blank)
{
    dump_indent(sink, col);
    sink_puts(sink, ")\n");
    if (blank)
	sink_putc(sink, '\n');
}

// A CYCLE. PHP prints the type word on the entry line, then `*RECURSION*` on
// its own line indented by exactly ONE space — at every depth, not stepped with
// the frame. Verified at depth 1 and depth 2 (tmp/or_value.php, cat -A):
//
//         [self] => Array$
//  *RECURSION*$
//
// That one space is PHP's, not a typo, and there is NO "(" block and no
// trailing blank line: the marker replaces the whole nested frame. Reached only
// by the RUNTIME value walk — a cycle needs aliasing, which a compile-time type
// walk cannot express.
void __madc_dump_pr_recursion(void *sink, const char *word)
{
    sink_puts(sink, word ? word : "");
    sink_puts(sink, "\n *RECURSION*\n");
}

// An opaque typed instance under print_r. PHP has no equivalent — there are no
// readable properties to list — so this reports the identity it does have. The
// type_id is printed as a NUMBER because no type-id -> name registry exists at
// run time yet (the segmented typeid table,
// docs/plans/2026-06-12-type-table-value-abi-design.md §3); when one lands, the
// type's name belongs here. Saying "Object" and stopping would be the quiet
// guess this arc refuses to make.
void __madc_dump_pr_instance(void *sink, unsigned type_id)
{
    sink_printf(sink, "instance#%u", type_id);
}

// ---------------------------------------------------------------------------
// var_dump
// ---------------------------------------------------------------------------
// Same walk, different frame. PHP's var_dump indents 2 per level, puts the key
// and the value on SEPARATE lines at the same column, terminates every value
// line itself, and adds no blank line after a nested block:
//
//     array(2) {$
//       [0]=>$
//       int(1)$
//       [1]=>$
//       array(2) {$
//         ["x"]=>$
//         int(3)$
//       }$
//     }$
//
// madc's ONE deliberate divergence is the type word: the REAL C/C++/madc type,
// not a simulated PHP one — `double` not `float`, `long` not `int`, `char *(2)`
// not `string(2)`, `struct Point(2)` not `object(Point)#1 (2)`. PHP's object
// handle (#1) is dropped: it identifies a PHP object instance and means nothing
// here. Captured from php-cli 8.3.6 with cat -A (tmp/or_vd.php).

// PHP's NULL line — var_dump's one retained PHP word, because "no value" is not
// a C type. THE owner of that spelling: a null `char *` and a value of kind null
// both route here, so the two can never disagree.
void __madc_dump_vd_null(void *sink, int col)
{
    dump_indent(sink, col);
    sink_puts(sink, "NULL\n");
}

// A cycle: the marker alone, at the value line's own column (which is where the
// key line above it also sits). Captured from php-cli 8.3.6 — see
// __madc_dump_pr_recursion for print_r's stranger shape.
void __madc_dump_vd_recursion(void *sink, int col)
{
    dump_indent(sink, col);
    sink_puts(sink, "*RECURSION*\n");
}

// An enum. PHP 8.1 renders one as enum(Tag::CASE) — which already names the real
// type and the real enumerator, so var_dump has nothing to diverge from here for
// once. Captured from php-cli 8.3.6 (tmp/or/enum.php, cat -A).
//
// A C enum value need NOT name an enumerator: `(Color)3` is legal C and PHP has
// no way to express it. That case reports the type and the value —
// `enum Color(3)` — the same <type>(<n>) shape every other var_dump line uses,
// rather than inventing an enumerator name for a value that has none.
void __madc_dump_vd_enum(void *sink, int col, const char *tag,
			 const char *name, long long v)
{
    dump_indent(sink, col);
    if (name && *name) {
	sink_puts(sink, "enum(");
	sink_puts(sink, tag ? tag : "?");
	sink_puts(sink, "::");
	sink_puts(sink, name);
	sink_puts(sink, ")\n");
	return;
    }
    sink_puts(sink, "enum ");
    sink_puts(sink, tag ? tag : "?");
    sink_printf(sink, "(%lld)\n", v);
}

// An opaque typed instance: its payload SIZE in bytes, then its type id. The
// count in parentheses is bytes here, where PHP's string(2) counts characters
// and array(5) counts elements — an opaque payload has neither. See
// __madc_dump_pr_instance for why the id is a number and not a name.
void __madc_dump_vd_instance(void *sink, int col, long long size,
			     unsigned type_id)
{
    dump_indent(sink, col);
    sink_printf(sink, "instance(%lld) #%u\n", size, type_id);
}

void __madc_dump_vd_head(void *sink, int col, const char *word, long long count)
{
    dump_indent(sink, col);
    sink_puts(sink, word ? word : "");
    sink_printf(sink, "(%lld) {\n", count);
}

void __madc_dump_vd_key(void *sink, int col, const char *key)
{
    // The caller supplies the quotes for this form: a member key is a literal
    // that already carries them, along with the access suffix that goes INSIDE
    // the brackets (["priv":"Foo":private]).
    __madc_dump_vd_key_open(sink, col, 0);
    sink_puts(sink, key ? key : "");
    __madc_dump_vd_key_close(sink, 0);
}

// A container key, rendered between the two. var_dump quotes a STRING key and
// not an integral one (php-cli 8.3.6: ["b"]=> vs [3]=>), and the compiler knows
// which from the key's type — so `quote` is a compile-time flag.
void __madc_dump_vd_key_open(void *sink, int col, int quote)
{
    dump_indent(sink, col);
    sink_putc(sink, '[');
    if (quote)
	sink_putc(sink, '"');
}

void __madc_dump_vd_key_close(void *sink, int quote)
{
    if (quote)
	sink_putc(sink, '"');
    sink_puts(sink, "]=>\n");
}

void __madc_dump_vd_key_idx(void *sink, int col, long long idx)
{
    dump_indent(sink, col);
    sink_printf(sink, "[%lld]=>\n", idx);
}

void __madc_dump_vd_tail(void *sink, int col)
{
    dump_indent(sink, col);
    sink_puts(sink, "}\n");
}

void __madc_dump_vd_i64(void *sink, int col, const char *ty, long long v,
			int is_unsigned)
{
    dump_indent(sink, col);
    sink_puts(sink, ty ? ty : "");
    if (is_unsigned)
	sink_printf(sink, "(%llu)\n", (unsigned long long)v);
    else
	sink_printf(sink, "(%lld)\n", v);
}

void __madc_dump_vd_f64(void *sink, int col, const char *ty, double v)
{
    dump_indent(sink, col);
    sink_puts(sink, ty ? ty : "");
    sink_putc(sink, '(');
    __madc_dump_pr_f64(sink, v);        /* one float format, both flavors */
    sink_puts(sink, ")\n");
}

void __madc_dump_vd_bool(void *sink, int col, const char *ty, int v)
{
    dump_indent(sink, col);
    sink_puts(sink, ty ? ty : "");
    sink_puts(sink, v ? "(true)\n" : "(false)\n");
}

// A char's value line names the character when it is printable and the byte
// otherwise: char('A') vs char(10). PHP has no char type, so C is the oracle
// and a non-printable byte must not be written raw into the output.
void __madc_dump_vd_char(void *sink, int col, const char *ty, int c)
{
    unsigned char b = (unsigned char)c;
    dump_indent(sink, col);
    sink_puts(sink, ty ? ty : "");
    if (b >= 0x20 && b < 0x7f)
	sink_printf(sink, "('%c')\n", b);
    else
	sink_printf(sink, "(%u)\n", (unsigned)b);
}

// A NULL pointer is PHP's null; __madc_dump_vd_null owns that line.
void __madc_dump_vd_cstr(void *sink, int col, const char *ty, const char *s)
{
    if (!s) {
	__madc_dump_vd_null(sink, col);
	return;
    }
    dump_indent(sink, col);
    sink_puts(sink, ty ? ty : "");
    sink_printf(sink, "(%llu) \"", (unsigned long long)strlen(s));
    sink_puts(sink, s);
    sink_puts(sink, "\"\n");
}

// A CONTAINER rendered as text (std::string, vector<char>): the frame only —
// the characters between the quotes are written one at a time by the generated
// loop, through __madc_dump_pr_char. The length comes from the container's own
// size(), so nothing here scans for a NUL.
void __madc_dump_vd_text_open(void *sink, int col, const char *ty, long long len)
{
    dump_indent(sink, col);
    sink_puts(sink, ty ? ty : "");
    sink_printf(sink, "(%lld) \"", len);
}

void __madc_dump_vd_text_close(void *sink)
{
    sink_puts(sink, "\"\n");
}

void __madc_dump_vd_cstr_n(void *sink, int col, const char *ty, const char *s,
			   long long n)
{
    long long len = 0;
    if (!s) {
	__madc_dump_vd_null(sink, col);
	return;
    }
    dump_indent(sink, col);
    while (len < n && s[len])
	len++;
    sink_puts(sink, ty ? ty : "");
    sink_printf(sink, "(%lld) \"", len);
    sink_write(sink, s, (size_t)len);
    sink_puts(sink, "\"\n");
}
