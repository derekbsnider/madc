/* rt_format.c — the std::format / std::print / std::println engine.
 *
 * ONE implementation of the std format-spec mini-language, shared by the
 * compile-time checker (cir_builder validates literal format strings by
 * calling the parser/iterator here) and the runtime emitters the lowered
 * code calls. The contract and the pinned oracle behaviors are documented
 * in rt_format.h; every rule below is enforced by
 * tests/unit/rt_format_oracle.inc (generated from libstdc++ std::format by
 * scripts/gen_format_oracle.cpp) — when a comment says "oracle", that file
 * is the evidence.
 *
 * Strict C11, libc only (snprintf/strtod/malloc), no compiler builtins —
 * ledger-lane rules (scripts/ledger_sources.txt). Output goes through the
 * dump runtime's byte sink (rt_dump.h): NULL -> stdout, else the growable
 * capture buffer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "rt_dump.h"
#include "rt_format.h"

/* ------------------------------------------------------------------------- */
/* small helpers                                                              */
/* ------------------------------------------------------------------------- */

static int fmt_is_digit(char c)
{
	return c >= '0' && c <= '9';	/* never ctype: locale-independent */
}

static int fmt_is_align(char c)
{
	return c == '<' || c == '>' || c == '^';
}

/* A format that CANNOT continue says so in the output, where the value would
 * have been — the same loud-marker policy as __madc_dump_fail, with its own
 * spelling so a reader knows which machinery failed. Reaching this at run
 * time means a spec escaped compile-time validation. */
static void fmt_fail(void *sink, const char *what)
{
	__madc_dump_raw(sink, "[madc format failed: ", 21);
	__madc_dump_raw(sink, what, (long long)strlen(what));
	__madc_dump_raw(sink, "]", 1);
}

/* Emit `count` copies of the fill byte, chunked. */
static void fmt_pad(void *sink, unsigned char fill, long long count)
{
	char chunk[64];
	memset(chunk, fill, sizeof chunk);
	while ( count > 0 )
	{
		long long n = count > (long long)sizeof chunk
			    ? (long long)sizeof chunk : count;
		__madc_dump_raw(sink, chunk, n);
		count -= n;
	}
}

/* Fill-and-align a fully rendered body. `defalign` is the type's default
 * ('<' for strings/chars/bool-text, '>' for numbers). */
static void fmt_emit_aligned(void *sink, const madc_fmt_spec *s,
			     char defalign, const char *body, long long blen)
{
	char align = s->align ? s->align : defalign;
	long long width = s->width < 0 ? 0 : s->width;
	long long lead = 0, trail = 0;

	if ( width > blen )
	{
		long long slack = width - blen;
		if ( align == '<' )
			trail = slack;
		else if ( align == '>' )
			lead = slack;
		else	/* '^': shorter run first (std: extra fill on the right) */
		{
			lead = slack / 2;
			trail = slack - lead;
		}
	}
	fmt_pad(sink, s->fill, lead);
	__madc_dump_raw(sink, body, blen);
	fmt_pad(sink, s->fill, trail);
}

/* Zero-pad a numeric body whose sign (and only its sign) may lead it: the
 * zeros go BETWEEN the sign/prefix head and the digits. `headlen` counts the
 * bytes that stay in front of the zeros. Callers apply the std rule that an
 * explicit align disables zero-padding. */
static void fmt_emit_zero_padded(void *sink, const madc_fmt_spec *s,
				 const char *body, long long blen,
				 long long headlen)
{
	__madc_dump_raw(sink, body, headlen);
	fmt_pad(sink, '0', (long long)s->width - blen);
	__madc_dump_raw(sink, body + headlen, blen - headlen);
}

/* ------------------------------------------------------------------------- */
/* the format-spec parser                                                     */
/* ------------------------------------------------------------------------- */

/* Parse a nonnegative decimal, rejecting overflow past the std-ish cap. */
static const char *fmt_parse_number(const char *spec, long long n,
				    long long *i, int *out)
{
	long long v = 0;
	while ( *i < n && fmt_is_digit(spec[*i]) )
	{
		v = v * 10 + (spec[*i] - '0');
		if ( v > 100000000 )
			return "format-spec width or precision is too large";
		++*i;
	}
	*out = (int)v;
	return 0;
}

const char *__madc_fmt_parse_spec(const char *spec, long long n,
				  madc_fmt_spec *out)
{
	long long i = 0;

	out->fill = ' ';
	out->align = 0;
	out->sign = 0;
	out->alt = 0;
	out->zero = 0;
	out->width = -1;
	out->precision = -1;
	out->type = 0;

	/* [[fill]align] — fill is any byte except '{'/'}', recognized only by
	 * the align char that follows it. */
	if ( n - i >= 2 && fmt_is_align(spec[i + 1]) )
	{
		if ( spec[i] == '{' || spec[i] == '}' )
			return "'{' and '}' cannot be fill characters";
		out->fill = (unsigned char)spec[i];
		out->align = spec[i + 1];
		i += 2;
	}
	else if ( n - i >= 1 && fmt_is_align(spec[i]) )
	{
		out->align = spec[i];
		++i;
	}

	/* [sign] — '-' is the default and parses to 0. */
	if ( i < n && (spec[i] == '+' || spec[i] == ' ') )
		out->sign = spec[i++];
	else if ( i < n && spec[i] == '-' )
		++i;

	if ( i < n && spec[i] == '#' )
	{
		out->alt = 1;
		++i;
	}
	if ( i < n && spec[i] == '0' )
	{
		out->zero = 1;
		++i;
	}

	/* [width] — a leading '0' belongs to the zero flag above, so a width
	 * here starts 1-9. '{' would be width-from-argument. */
	if ( i < n && spec[i] == '{' )
		return "width from an argument ({}) is not supported yet";
	if ( i < n && fmt_is_digit(spec[i]) )
	{
		const char *err = fmt_parse_number(spec, n, &i, &out->width);
		if ( err )
			return err;
	}

	/* [.precision] */
	if ( i < n && spec[i] == '.' )
	{
		++i;
		if ( i < n && spec[i] == '{' )
			return "precision from an argument ({}) is not supported yet";
		if ( i >= n || !fmt_is_digit(spec[i]) )
			return "missing precision after '.'";
		{
			const char *err = fmt_parse_number(spec, n, &i,
							   &out->precision);
			if ( err )
				return err;
		}
	}

	if ( i < n && spec[i] == 'L' )
		return "locale-specific formatting (L) is not supported";

	if ( i < n )
		out->type = spec[i++];

	if ( i != n )
		return "trailing characters in format-spec";
	return 0;
}

/* ------------------------------------------------------------------------- */
/* format-string iteration                                                    */
/* ------------------------------------------------------------------------- */

long long __madc_fmt_next(const char *fmt, long long n, long long pos,
			  madc_fmt_item *out, const char **err)
{
	*err = 0;
	if ( pos >= n )
		return -1;

	out->kind = MADC_FMT_TEXT;
	out->text = fmt + pos;
	out->text_n = 0;
	out->arg_id = -1;
	out->spec = 0;
	out->spec_n = 0;

	/* "{{" / "}}" — a one-byte literal run holding the brace itself. */
	if ( pos + 1 < n && fmt[pos] == fmt[pos + 1]
	  && (fmt[pos] == '{' || fmt[pos] == '}') )
	{
		out->text_n = 1;
		return pos + 2;
	}
	if ( fmt[pos] == '}' )
	{
		*err = "unmatched '}' in format string";
		return -2;
	}

	if ( fmt[pos] != '{' )
	{
		/* a literal run up to the next brace of either kind */
		long long i = pos;
		while ( i < n && fmt[i] != '{' && fmt[i] != '}' )
			++i;
		out->text_n = i - pos;
		return i;
	}

	/* a replacement field: { [arg-id] [: spec] } */
	{
		long long i = pos + 1;
		out->kind = MADC_FMT_FIELD;
		if ( i < n && fmt_is_digit(fmt[i]) )
		{
			int id = 0;
			const char *e = fmt_parse_number(fmt, n, &i, &id);
			if ( e )
			{
				*err = "argument index is too large";
				return -2;
			}
			out->arg_id = id;
		}
		if ( i < n && fmt[i] == ':' )
		{
			++i;
			out->spec = fmt + i;
			/* the spec runs to the matching '}' — no nested
			 * fields yet (width-from-argument is unsupported,
			 * the spec parser rejects '{'). */
			while ( i < n && fmt[i] != '}' && fmt[i] != '{' )
				++i;
			out->spec_n = (fmt + i) - out->spec;
		}
		if ( i >= n || fmt[i] != '}' )
		{
			*err = "unterminated replacement field ('}' expected)";
			return -2;
		}
		return i + 1;
	}
}

/* ------------------------------------------------------------------------- */
/* integers (and everything that routes through them)                         */
/* ------------------------------------------------------------------------- */

static const char *fmt_digits_lc = "0123456789abcdef";
static const char *fmt_digits_uc = "0123456789ABCDEF";

/* Render sign+prefix+digits and emit with the std padding rules.
 * `neg` carries the sign separately so LLONG_MIN's magnitude fits. */
static void fmt_int_core(void *sink, const madc_fmt_spec *s,
			 unsigned long long mag, int neg)
{
	char body[96];
	long long headlen = 0, blen = 0;
	unsigned base = 10;
	const char *digits = fmt_digits_lc;
	char defalign = '>';

	switch ( s->type )
	{
	case 0: case 'd':
		break;
	case 'b': case 'B':
		base = 2;
		break;
	case 'o':
		base = 8;
		break;
	case 'x':
		base = 16;
		break;
	case 'X':
		base = 16;
		digits = fmt_digits_uc;
		break;
	case 'c':
		/* the value must be representable in char — SIGNED here, so
		 * -128..127 (oracle: -42 formats as byte \326, 255 throws).
		 * The lowering rejects constants outside the range; a runtime
		 * value dodging that gets the loud marker. */
		if ( neg ? mag > 128 : mag > 127 )
		{
			fmt_fail(sink, "integer not representable as character");
			return;
		}
		body[0] = (char)(unsigned char)
			  ((neg ? 0 - (long long)mag : (long long)mag) & 0xff);
		fmt_emit_aligned(sink, s, '<', body, 1);
		return;
	default:
		fmt_fail(sink, "invalid presentation type for an integer");
		return;
	}

	if ( neg )
		body[blen++] = '-';
	else if ( s->sign )
		body[blen++] = s->sign;
	if ( s->alt )
	{
		/* oracle: "#x" of -42 -> "-0x2a"; "#o" of 0 stays "0" */
		if ( base == 2 )
		{
			body[blen++] = '0';
			body[blen++] = s->type;	/* 'b' or 'B' */
		}
		else if ( base == 16 )
		{
			body[blen++] = '0';
			body[blen++] = s->type;	/* 'x' or 'X' */
		}
		else if ( base == 8 && mag != 0 )
			body[blen++] = '0';
	}
	headlen = blen;

	{
		char rev[64];
		int rn = 0;
		do
		{
			rev[rn++] = digits[mag % base];
			mag /= base;
		} while ( mag );
		while ( rn )
			body[blen++] = rev[--rn];
	}

	if ( s->zero && !s->align && (long long)s->width > blen )
		fmt_emit_zero_padded(sink, s, body, blen, headlen);
	else
		fmt_emit_aligned(sink, s, defalign, body, blen);
}

void __madc_fmt_i64(void *sink, const char *spec, long long spec_n,
		    long long v)
{
	madc_fmt_spec s;
	const char *err = __madc_fmt_parse_spec(spec, spec_n, &s);
	if ( err )
	{
		fmt_fail(sink, err);
		return;
	}
	if ( v < 0 )
		fmt_int_core(sink, &s, (unsigned long long)-(v + 1) + 1ull, 1);
	else
		fmt_int_core(sink, &s, (unsigned long long)v, 0);
}

void __madc_fmt_u64(void *sink, const char *spec, long long spec_n,
		    unsigned long long v)
{
	madc_fmt_spec s;
	const char *err = __madc_fmt_parse_spec(spec, spec_n, &s);
	if ( err )
	{
		fmt_fail(sink, err);
		return;
	}
	fmt_int_core(sink, &s, v, 0);
}

/* ------------------------------------------------------------------------- */
/* floating point                                                             */
/* ------------------------------------------------------------------------- */

/* Emit a finished float body honoring the zero flag (zeros go after a leading
 * sign; std ignores the flag when an align is given — callers pass finite
 * bodies only, inf/nan take the aligned path directly). */
static void fmt_f64_emit(void *sink, const madc_fmt_spec *s,
			 const char *body, long long blen)
{
	long long headlen = (blen > 0 && (body[0] == '-' || body[0] == '+'
					  || body[0] == ' ')) ? 1 : 0;
	if ( s->zero && !s->align && (long long)s->width > blen )
		fmt_emit_zero_padded(sink, s, body, blen, headlen);
	else
		fmt_emit_aligned(sink, s, '>', body, blen);
}

/* The default ({}) float form: SHORTEST ROUND-TRIP digits, presented fixed or
 * scientific by rendered length with fixed winning ties (oracle: "10000" and
 * "0.001" stay fixed, "1e+05"/"1e-04" go scientific). The shortest digit
 * string comes from the classic portable loop — %.{p-1}e at rising p until
 * strtod round-trips bit-exactly — which equals to_chars output BY SPEC
 * (to_chars general is defined in terms of %g at shortest precision). */
static void fmt_f64_shortest(void *sink, const madc_fmt_spec *s, double v)
{
	char sci[64];
	char fixed[384];
	char digits[24];
	long long dn = 0, fn = 0;
	long long exp10 = 0;
	int p, neg = 0;
	const char *c;

	for ( p = 1; p <= 17; ++p )
	{
		double back;
		snprintf(sci, sizeof sci, "%.*e", p - 1, v);
		back = strtod(sci, 0);
		if ( memcmp(&back, &v, sizeof v) == 0 )
			break;
	}

	/* pick the sci body apart: [-]d[.ddd]e±XX */
	c = sci;
	if ( *c == '-' )
	{
		neg = 1;
		++c;
	}
	for ( ; *c && *c != 'e'; ++c )
		if ( *c != '.' )
			digits[dn++] = *c;
	if ( *c == 'e' )
		exp10 = strtoll(c + 1, 0, 10);

	/* build the fixed twin from the SAME digits (never %f: its exact
	 * decimal expansion differs from shortest-digits-plus-zeros) */
	if ( neg )
		fixed[fn++] = '-';
	if ( exp10 >= dn - 1 )
	{
		long long z;
		memcpy(fixed + fn, digits, (size_t)dn);
		fn += dn;
		for ( z = exp10 - (dn - 1); z > 0; --z )
			fixed[fn++] = '0';
	}
	else if ( exp10 >= 0 )
	{
		memcpy(fixed + fn, digits, (size_t)(exp10 + 1));
		fn += exp10 + 1;
		fixed[fn++] = '.';
		memcpy(fixed + fn, digits + exp10 + 1,
		       (size_t)(dn - exp10 - 1));
		fn += dn - exp10 - 1;
	}
	else
	{
		long long z;
		fixed[fn++] = '0';
		fixed[fn++] = '.';
		for ( z = -exp10 - 1; z > 0; --z )
			fixed[fn++] = '0';
		memcpy(fixed + fn, digits, (size_t)dn);
		fn += dn;
	}

	/* an explicit '+'/' ' sign applies to either presentation */
	if ( !neg && s->sign )
	{
		char signed_body[392];
		const char *sel = fixed;
		long long seln = fn;
		if ( fn > (long long)strlen(sci) )
		{
			sel = sci;
			seln = (long long)strlen(sci);
		}
		signed_body[0] = s->sign;
		memcpy(signed_body + 1, sel, (size_t)seln);
		fmt_f64_emit(sink, s, signed_body, seln + 1);
		return;
	}

	if ( fn <= (long long)strlen(sci) )
		fmt_f64_emit(sink, s, fixed, fn);
	else
		fmt_f64_emit(sink, s, sci, (long long)strlen(sci));
}

/* Precision-less 'a'/'A': to_chars hex-shortest, built bit-exactly. printf %a
 * agrees for NORMAL values but prints denormals in the 0.xxxp-1022 form where
 * to_chars normalizes to a leading nonzero digit (oracle: denormal min is
 * "1p-1074", not "0.0000000000001p-1022") — so the digits come from the bits,
 * never from %a. Trailing zero nibbles strip; '#' keeps the point. */
static void fmt_f64_hex_shortest(void *sink, const madc_fmt_spec *s,
				 double v, int upper)
{
	unsigned long long bits, frac;
	long long exp2;
	char lead;
	char body[48];
	long long blen = 0;
	const char *dg = upper ? fmt_digits_uc : fmt_digits_lc;

	memcpy(&bits, &v, sizeof bits);
	{
		unsigned long long man = bits & 0xfffffffffffffull;
		int be = (int)((bits >> 52) & 0x7ff);
		if ( be != 0 )
		{
			lead = '1';
			frac = man;
			exp2 = be - 1023;
		}
		else if ( man == 0 )
		{
			lead = '0';	/* ±zero: "0p+0" */
			frac = 0;
			exp2 = 0;
		}
		else
		{
			int k = 51;
			while ( !((man >> k) & 1) )
				--k;
			lead = '1';
			frac = (man << (52 - k)) & 0xfffffffffffffull;
			exp2 = k - 1074;
		}
	}

	if ( bits >> 63 )
		body[blen++] = '-';
	else if ( s->sign )
		body[blen++] = s->sign;
	body[blen++] = lead;
	{
		char nib[13];
		int n = 0, i;
		for ( i = 12; i >= 0; --i )
			nib[12 - i] = dg[(frac >> (4 * i)) & 0xf];
		for ( n = 13; n > 0 && nib[n - 1] == dg[0]; --n )
			;	/* strip trailing zero nibbles */
		if ( n > 0 || s->alt )
			body[blen++] = '.';
		memcpy(body + blen, nib, (size_t)n);
		blen += n;
	}
	body[blen++] = upper ? 'P' : 'p';
	body[blen++] = exp2 < 0 ? '-' : '+';
	{
		char rev[8];
		int rn = 0;
		unsigned long long mag = exp2 < 0
			? (unsigned long long)-exp2 : (unsigned long long)exp2;
		do
		{
			rev[rn++] = (char)('0' + mag % 10);
			mag /= 10;
		} while ( mag );
		while ( rn )
			body[blen++] = rev[--rn];
	}
	fmt_f64_emit(sink, s, body, blen);
}

void __madc_fmt_f64(void *sink, const char *spec, long long spec_n, double v)
{
	madc_fmt_spec s;
	char type;
	const char *err = __madc_fmt_parse_spec(spec, spec_n, &s);
	if ( err )
	{
		fmt_fail(sink, err);
		return;
	}
	type = s.type;
	switch ( type )
	{
	case 0:
	case 'f': case 'F': case 'e': case 'E':
	case 'g': case 'G': case 'a': case 'A':
		break;
	default:
		fmt_fail(sink, "invalid presentation type for a float");
		return;
	}

	/* inf/nan: lowercase for the lowercase presentations and the default,
	 * uppercase for F/E/G/A; the zero flag and precision are ignored
	 * (oracle: "010.3f" of inf -> "       inf"). */
	if ( isnan(v) || isinf(v) )
	{
		char body[8];
		long long blen = 0;
		int upper = type == 'F' || type == 'E' || type == 'G'
			 || type == 'A';
		if ( signbit(v) )
			body[blen++] = '-';
		else if ( s.sign )
			body[blen++] = s.sign;
		if ( isnan(v) )
		{
			memcpy(body + blen, upper ? "NAN" : "nan", 3);
			blen += 3;
		}
		else
		{
			memcpy(body + blen, upper ? "INF" : "inf", 3);
			blen += 3;
		}
		fmt_emit_aligned(sink, &s, '>', body, blen);
		return;
	}

	if ( type == 0 && s.precision < 0 )
	{
		fmt_f64_shortest(sink, &s, v);
		return;
	}
	if ( (type == 'a' || type == 'A') && s.precision < 0 )
	{
		fmt_f64_hex_shortest(sink, &s, v, type == 'A');
		return;
	}

	/* delegate the digit production to snprintf — the '{:.N}' typeless
	 * form is general presentation ('g' family) at that precision */
	{
		char pf[16];
		char stackbuf[352];
		char *body = stackbuf;
		long long need, blen;
		int pi = 0;
		char t = type == 0 ? 'g' : type;
		pf[pi++] = '%';
		if ( s.sign )
			pf[pi++] = s.sign;
		if ( s.alt )
			pf[pi++] = '#';
		pf[pi++] = '.';
		pf[pi++] = '*';
		pf[pi++] = t;
		pf[pi] = 0;
		{
			int prec = s.precision >= 0 ? s.precision : 6;
			need = snprintf(0, 0, pf, prec, v);
			if ( need + 1 > (long long)sizeof stackbuf )
			{
				body = (char *)malloc((size_t)need + 1);
				if ( !body )
				{
					fmt_fail(sink, "out of memory");
					return;
				}
			}
			blen = snprintf(body, (size_t)need + 1, pf, prec, v);
		}

		/* 'a'/'A' are printf %a/%A with the 0x/0X prefix stripped
		 * (oracle: "{:a}" of 3.5 -> "1.cp+1") */
		if ( t == 'a' || t == 'A' )
		{
			long long at = (body[0] == '-' || body[0] == '+'
					|| body[0] == ' ') ? 1 : 0;
			memmove(body + at, body + at + 2,
				(size_t)(blen - at - 2) + 1);
			blen -= 2;
		}

		fmt_f64_emit(sink, &s, body, blen);
		if ( body != stackbuf )
			free(body);
	}
}

/* ------------------------------------------------------------------------- */
/* strings, chars, bool, pointers                                             */
/* ------------------------------------------------------------------------- */

void __madc_fmt_str_n(void *sink, const char *spec, long long spec_n,
		      const char *str, long long sn)
{
	madc_fmt_spec s;
	const char *err = __madc_fmt_parse_spec(spec, spec_n, &s);
	if ( err )
	{
		fmt_fail(sink, err);
		return;
	}
	if ( s.type != 0 && s.type != 's' )
	{
		fmt_fail(sink, "invalid presentation type for a string");
		return;
	}
	/* BYTE semantics throughout (pinned against libstdc++ 13): width
	 * counts bytes, precision truncates bytes — even mid-UTF-8-sequence,
	 * exactly as the oracle does. */
	if ( s.precision >= 0 && sn > (long long)s.precision )
		sn = s.precision;
	fmt_emit_aligned(sink, &s, '<', str, sn);
}

void __madc_fmt_cstr(void *sink, const char *spec, long long spec_n,
		     const char *str)
{
	__madc_fmt_str_n(sink, spec, spec_n, str,
			 str ? (long long)strlen(str) : 0);
}

void __madc_fmt_char(void *sink, const char *spec, long long spec_n, int c)
{
	madc_fmt_spec s;
	const char *err = __madc_fmt_parse_spec(spec, spec_n, &s);
	if ( err )
	{
		fmt_fail(sink, err);
		return;
	}
	if ( s.type == 0 || s.type == 'c' )
	{
		char body = (char)c;
		fmt_emit_aligned(sink, &s, '<', &body, 1);
		return;
	}
	/* integer presentations format the character's VALUE */
	if ( c < 0 )
		fmt_int_core(sink, &s, (unsigned long long)-(long long)c, 1);
	else
		fmt_int_core(sink, &s, (unsigned long long)c, 0);
}

void __madc_fmt_bool(void *sink, const char *spec, long long spec_n, int v)
{
	madc_fmt_spec s;
	const char *err = __madc_fmt_parse_spec(spec, spec_n, &s);
	if ( err )
	{
		fmt_fail(sink, err);
		return;
	}
	if ( s.type == 0 || s.type == 's' )
	{
		if ( v )
			fmt_emit_aligned(sink, &s, '<', "true", 4);
		else
			fmt_emit_aligned(sink, &s, '<', "false", 5);
		return;
	}
	fmt_int_core(sink, &s, v ? 1ull : 0ull, 0);
}

void __madc_fmt_ptr(void *sink, const char *spec, long long spec_n,
		    const void *p)
{
	madc_fmt_spec s;
	const char *err = __madc_fmt_parse_spec(spec, spec_n, &s);
	if ( err )
	{
		fmt_fail(sink, err);
		return;
	}
	if ( s.type != 0 && s.type != 'p' )
	{
		fmt_fail(sink, "invalid presentation type for a pointer");
		return;
	}
	{
		char body[24];
		long long blen = 0;
		unsigned long long mag = (unsigned long long)(uintptr_t)p;
		char rev[16];
		int rn = 0;
		body[blen++] = '0';
		body[blen++] = 'x';
		do
		{
			rev[rn++] = fmt_digits_lc[mag & 0xf];
			mag >>= 4;
		} while ( mag );
		while ( rn )
			body[blen++] = rev[--rn];
		fmt_emit_aligned(sink, &s, '>', body, blen);
	}
}

void __madc_fmt_text(void *sink, const char *p, long long n)
{
	__madc_dump_raw(sink, p, n);
}
