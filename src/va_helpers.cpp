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
