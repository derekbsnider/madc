///////////////////////////////////////////////////////////////////////////
//                                                                       //
// va_helpers.cpp — madc varargs support functions                       //
//                                                                       //
// These C functions are compiled by gcc and linked into the madc binary. //
// They bridge madc's packed int64_t[] varargs buffer to libc's printf    //
// family by parsing the format string and calling sprintf per-specifier. //
//                                                                       //
///////////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>

struct madc_timeval
{
    int64_t tv_sec;
    int64_t tv_usec;
};

// Parse one format specifier from *pp (starting at '%'), advance *pp past it,
// copy the specifier into mini_fmt, and return the conversion character.
static char parse_spec(const char **pp, char *mini_fmt, size_t mini_size)
{
    const char *start = *pp;
    const char *p = start + 1; // skip '%'

    // literal %%
    if ( *p == '%' ) { *pp = p + 1; mini_fmt[0] = '%'; mini_fmt[1] = '%'; mini_fmt[2] = '\0'; return '%'; }

    // flags
    while ( *p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0' ) p++;
    // width (may be '*')
    if ( *p == '*' ) p++; else while ( *p >= '0' && *p <= '9' ) p++;
    // precision
    if ( *p == '.' ) { p++; if ( *p == '*' ) p++; else while ( *p >= '0' && *p <= '9' ) p++; }
    // length modifiers
    while ( *p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'q' ) p++;

    char spec = *p++;
    size_t len = (size_t)(p - start);
    if ( len >= mini_size ) len = mini_size - 1;
    memcpy(mini_fmt, start, len);
    mini_fmt[len] = '\0';
    *pp = p;
    return spec;
}

// vsprintf replacement: takes args as a packed int64_t array.
// The format string drives unpacking — each %d/%s/%f etc. consumes one slot.
extern "C" int __madc_vsprintf(char *buf, const char *fmt, int64_t *args)
{
    char *out = buf;
    const char *p = fmt;
    int ai = 0;
    char mini_fmt[128];

    while ( *p )
    {
	if ( *p != '%' ) { *out++ = *p++; continue; }

	char spec = parse_spec(&p, mini_fmt, sizeof(mini_fmt));
	if ( spec == '%' ) { *out++ = '%'; continue; }

	int written = 0;
	switch ( spec )
	{
	    case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'c':
		written = sprintf(out, mini_fmt, (long)args[ai++]);
		break;
	    case 's':
		written = sprintf(out, mini_fmt, (const char *)args[ai++]);
		break;
	    case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
	    {
		double d;
		memcpy(&d, &args[ai++], sizeof(double));
		written = sprintf(out, mini_fmt, d);
		break;
	    }
	    case 'p':
		written = sprintf(out, mini_fmt, (void *)args[ai++]);
		break;
	    case 'n':
		if ( args ) *(int *)(intptr_t)args[ai++] = (int)(out - buf);
		break;
	    default:
		written = sprintf(out, mini_fmt, args[ai++]);
		break;
	}
	out += written;
    }
    *out = '\0';
    return (int)(out - buf);
}

// vsnprintf replacement with size limit
extern "C" int __madc_vsnprintf(char *buf, size_t size, const char *fmt, int64_t *args)
{
    if ( size == 0 ) return 0;
    // Use a temporary buffer, then truncate
    char tmp[16384];
    int len = __madc_vsprintf(tmp, fmt, args);
    size_t copy = (size_t)len < size - 1 ? (size_t)len : size - 1;
    memcpy(buf, tmp, copy);
    buf[copy] = '\0';
    return len;
}

// vfprintf replacement
extern "C" int __madc_vfprintf(FILE *fp, const char *fmt, int64_t *args)
{
    char tmp[16384];
    int len = __madc_vsprintf(tmp, fmt, args);
    fputs(tmp, fp);
    return len;
}

// scanf-family format rewriter: madc's `int` is 64-bit, but C `%d` writes
// only 4 bytes — leaving the high 4 bytes of the destination slot at
// whatever they were before, which madc then reads back as a 64-bit value
// and gets a corrupted positive number for negative inputs (or vice versa).
// We rewrite each integer specifier (d/i/u/o/x/X/n) without an explicit
// length modifier to its `l`-prefixed form (%ld, %lu, etc.), so libc writes
// 8 bytes — matching madc's int slot width.
//
// Specifiers that already carry a length modifier (h, hh, l, ll, j, z, t)
// are left alone. %s, %c, %f, %lf, %p, %% pass through unchanged.
static void rewrite_scanf_format(const char *src, char *dst, size_t dst_size)
{
    const char *p = src;
    char *q = dst;
    char *end = dst + dst_size - 1;
    while ( *p && q < end )
    {
        if ( *p != '%' ) { *q++ = *p++; continue; }
        // copy '%'
        *q++ = *p++;
        // literal %%
        if ( *p == '%' ) { if ( q < end ) *q++ = *p; p++; continue; }
        // optional '*' suppression flag
        if ( *p == '*' ) { if ( q < end ) *q++ = *p++; }
        // optional max-field-width
        while ( *p >= '0' && *p <= '9' ) { if ( q < end ) *q++ = *p++; else { p++; } }
        // length modifier scan
        bool has_length = false;
        while ( *p == 'h' || *p == 'l' || *p == 'j' || *p == 'z' || *p == 't' || *p == 'L' || *p == 'q' )
        {
            has_length = true;
            if ( q < end ) *q++ = *p++; else { p++; }
        }
        // conversion char
        char spec = *p;
        if ( !has_length && (spec == 'd' || spec == 'i' || spec == 'u'
                          || spec == 'o' || spec == 'x' || spec == 'X'
                          || spec == 'n') )
        {
            if ( q < end ) *q++ = 'l';
        }
        if ( *p ) { if ( q < end ) *q++ = *p++; else p++; }
    }
    *q = '\0';
}

extern "C" int __madc_sscanf(const char *str, const char *fmt, ...)
{
    char new_fmt[2048];
    rewrite_scanf_format(fmt, new_fmt, sizeof(new_fmt));
    va_list ap;
    va_start(ap, fmt);
    int rc = vsscanf(str, new_fmt, ap);
    va_end(ap);
    return rc;
}

extern "C" int __madc_fscanf(FILE *fp, const char *fmt, ...)
{
    char new_fmt[2048];
    rewrite_scanf_format(fmt, new_fmt, sizeof(new_fmt));
    va_list ap;
    va_start(ap, fmt);
    int rc = vfscanf(fp, new_fmt, ap);
    va_end(ap);
    return rc;
}

extern "C" int __madc_scanf(const char *fmt, ...)
{
    char new_fmt[2048];
    rewrite_scanf_format(fmt, new_fmt, sizeof(new_fmt));
    va_list ap;
    va_start(ap, fmt);
    int rc = vscanf(new_fmt, ap);
    va_end(ap);
    return rc;
}

// fd_set bit-array helpers. Called by the FD_ZERO/FD_SET/FD_CLR/FD_ISSET
// macros in the embedded <sys/select.h>. The "set" pointer is the address
// of a `struct fd_set` (128 bytes, laid out as 16 int64s — identical to
// glibc's fd_set on x86-64).
extern "C" void __madc_fd_zero(void *set)
{
    memset(set, 0, 128);
}
extern "C" void __madc_fd_set(long fd, void *set)
{
    ((long *)set)[fd / 64] |= (1L << (fd % 64));
}
extern "C" void __madc_fd_clr(long fd, void *set)
{
    ((long *)set)[fd / 64] &= ~(1L << (fd % 64));
}
extern "C" long __madc_fd_isset(long fd, void *set)
{
    return (((long *)set)[fd / 64] >> (fd % 64)) & 1L;
}

extern "C" long __madc_timeval_sec(void *tv)
{
    return ((madc_timeval *)tv)->tv_sec;
}

extern "C" long __madc_timeval_usec(void *tv)
{
    return ((madc_timeval *)tv)->tv_usec;
}

extern "C" long __madc_timerisset(void *tv)
{
    madc_timeval *tp = (madc_timeval *)tv;
    return tp->tv_sec || tp->tv_usec;
}

extern "C" void __madc_timerclear(void *tv)
{
    madc_timeval *tp = (madc_timeval *)tv;
    tp->tv_sec = 0;
    tp->tv_usec = 0;
}

extern "C" void __madc_timeradd(void *left, void *right, void *result)
{
    madc_timeval *a = (madc_timeval *)left;
    madc_timeval *b = (madc_timeval *)right;
    madc_timeval *r = (madc_timeval *)result;
    r->tv_sec = a->tv_sec + b->tv_sec;
    r->tv_usec = a->tv_usec + b->tv_usec;
    if ( r->tv_usec >= 1000000 )
    {
        ++r->tv_sec;
        r->tv_usec -= 1000000;
    }
}

extern "C" void __madc_timersub(void *left, void *right, void *result)
{
    madc_timeval *a = (madc_timeval *)left;
    madc_timeval *b = (madc_timeval *)right;
    madc_timeval *r = (madc_timeval *)result;
    r->tv_sec = a->tv_sec - b->tv_sec;
    r->tv_usec = a->tv_usec - b->tv_usec;
    if ( r->tv_usec < 0 )
    {
        --r->tv_sec;
        r->tv_usec += 1000000;
    }
}
