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
