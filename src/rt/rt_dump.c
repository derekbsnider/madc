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
///////////////////////////////////////////////////////////////////////////
#include <stdio.h>
#include <string.h>

// stdout, unbuffered-order-wise, is shared with the C++ iostreams the rest of
// the runtime prints through (std::cout is sync_with_stdio by default), so a
// script may interleave `cout <<` and php::print_r freely.

// print_r of an integer: the decimal digits, NO newline and no type — PHP's
// print_r(42) is exactly "42". Signedness comes from the compiler, which knows
// the real type; the value rides as 64 bits either way.
void __madc_dump_pr_i64(long long v, int is_unsigned)
{
    if (is_unsigned)
	printf("%llu", (unsigned long long)v);
    else
	printf("%lld", v);
}

// print_r of a floating value: PHP's own double->string, which is `%.14G`
// (precision=14) PLUS a guaranteed decimal point in the mantissa of an
// exponent form — PHP prints 1.0E+25 where C's %G prints 1E+25.
void __madc_dump_pr_f64(double v)
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
	printf("%s", buf);
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
    printf("%s", out);
}

// print_r of a bool: PHP renders true as "1" and false as the EMPTY string.
void __madc_dump_pr_bool(int v)
{
    if (v)
	printf("1");
}

// print_r of a character: the byte itself, as a one-character string. PHP has
// no char type; a PHP developer handed chr(65) sees "A".
void __madc_dump_pr_char(int c)
{
    printf("%c", (unsigned char)c);
}

// print_r of a C string: the text. A NULL pointer is PHP's null, which print_r
// renders as the empty string.
void __madc_dump_pr_cstr(const char *s)
{
    if (s)
	printf("%s", s);
}

// print_r of a character ARRAY: text, bounded by the array's extent. A C array
// need not be NUL-terminated, so %s could read past it; stop at the extent or
// at the first NUL, whichever comes first.
void __madc_dump_pr_cstr_n(const char *s, long long n)
{
    long long i;
    if (!s)
	return;
    for (i = 0; i < n && s[i]; i++)
	putchar(s[i]);
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

static void dump_indent(int col)
{
    int i;
    for (i = 0; i < col; i++)
	putchar(' ');
}

// The aggregate's opening: its type word ("Array", "Point Object"), then "(".
void __madc_dump_pr_head(int col, const char *word)
{
    printf("%s\n", word ? word : "");
    dump_indent(col);
    printf("(\n");
}

// One entry's key: PHP spells a protected member "[name:protected]" and a
// private one "[name:Class:private]". The whole key text is a compile-time
// literal — the compiler knows the access and the declaring class.
void __madc_dump_pr_key(int col, const char *key)
{
    dump_indent(col);
    printf("[%s] => ", key ? key : "");
}

// An array element's key: the position.
void __madc_dump_pr_key_idx(int col, long long idx)
{
    dump_indent(col);
    printf("[%lld] => ", idx);
}

// End of a SCALAR entry. A nested aggregate ends itself (its ")" line plus the
// blank line), so this must not be emitted for one.
void __madc_dump_pr_nl(void)
{
    putchar('\n');
}

// The aggregate's close. `blank` adds PHP's trailing empty line, which every
// NESTED block gets and the outermost one does not.
void __madc_dump_pr_tail(int col, int blank)
{
    dump_indent(col);
    printf(")\n");
    if (blank)
	putchar('\n');
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

void __madc_dump_vd_head(int col, const char *word, long long count)
{
    dump_indent(col);
    printf("%s(%lld) {\n", word ? word : "", count);
}

void __madc_dump_vd_key(int col, const char *key)
{
    dump_indent(col);
    printf("[%s]=>\n", key ? key : "");
}

void __madc_dump_vd_key_idx(int col, long long idx)
{
    dump_indent(col);
    printf("[%lld]=>\n", idx);
}

void __madc_dump_vd_tail(int col)
{
    dump_indent(col);
    printf("}\n");
}

void __madc_dump_vd_i64(int col, const char *ty, long long v, int is_unsigned)
{
    dump_indent(col);
    if (is_unsigned)
	printf("%s(%llu)\n", ty ? ty : "", (unsigned long long)v);
    else
	printf("%s(%lld)\n", ty ? ty : "", v);
}

void __madc_dump_vd_f64(int col, const char *ty, double v)
{
    dump_indent(col);
    printf("%s(", ty ? ty : "");
    __madc_dump_pr_f64(v);              /* one float format, both flavors */
    printf(")\n");
}

void __madc_dump_vd_bool(int col, const char *ty, int v)
{
    dump_indent(col);
    printf("%s(%s)\n", ty ? ty : "", v ? "true" : "false");
}

// A char's value line names the character when it is printable and the byte
// otherwise: char('A') vs char(10). PHP has no char type, so C is the oracle
// and a non-printable byte must not be written raw into the output.
void __madc_dump_vd_char(int col, const char *ty, int c)
{
    unsigned char b = (unsigned char)c;
    dump_indent(col);
    if (b >= 0x20 && b < 0x7f)
	printf("%s('%c')\n", ty ? ty : "", b);
    else
	printf("%s(%u)\n", ty ? ty : "", (unsigned)b);
}

// A NULL pointer is PHP's null, and PHP's var_dump prints NULL for it — the one
// case where var_dump keeps PHP's word, because "no value" is not a C type.
void __madc_dump_vd_cstr(int col, const char *ty, const char *s)
{
    dump_indent(col);
    if (!s) {
	printf("NULL\n");
	return;
    }
    printf("%s(%llu) \"%s\"\n", ty ? ty : "", (unsigned long long)strlen(s), s);
}

void __madc_dump_vd_cstr_n(int col, const char *ty, const char *s, long long n)
{
    long long len = 0;
    dump_indent(col);
    if (!s) {
	printf("NULL\n");
	return;
    }
    while (len < n && s[len])
	len++;
    printf("%s(%lld) \"", ty ? ty : "", len);
    fwrite(s, 1, (size_t)len, stdout);
    printf("\"\n");
}
