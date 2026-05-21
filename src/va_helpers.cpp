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
#include <csetjmp>
#include <map>

struct madc_timeval
{
    int64_t tv_sec;
    int64_t tv_usec;
};

struct madc_jmp_slot
{
    jmp_buf env;
};

static std::map<void *, madc_jmp_slot *> &madc_jmp_slots()
{
    static std::map<void *, madc_jmp_slot *> slots;
    return slots;
}

static madc_jmp_slot *madc_jmp_slot_for(void *user_buf)
{
    std::map<void *, madc_jmp_slot *> &slots = madc_jmp_slots();
    std::map<void *, madc_jmp_slot *>::iterator it = slots.find(user_buf);
    if ( it != slots.end() )
	return it->second;
    madc_jmp_slot *slot = new madc_jmp_slot;
    slots[user_buf] = slot;
    return slot;
}

extern "C" void *__madc_jmpbuf_for(void *user_buf)
{
    return (void *)madc_jmp_slot_for(user_buf)->env;
}

extern "C" void __madc_builtin_longjmp(void *user_buf, int value)
{
    if ( value == 0 )
	value = 1;
    longjmp(madc_jmp_slot_for(user_buf)->env, value);
}

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

// vprintf replacement
extern "C" int __madc_vprintf(const char *fmt, int64_t *args)
{
    return __madc_vfprintf(stdout, fmt, args);
}

// With sizeof(int)==4 (LP64 ABI), libc's %d writes the correct 4 bytes
// into a standard int slot. No format rewriting is needed.
// The old rewrite_scanf_format shim was required when madc's int was
// 8 bytes — it promoted %d to %ld so libc would write 8 bytes.
// Now that int matches the platform ABI, the wrappers just forward
// to the real libc functions.

extern "C" int __madc_sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = vsscanf(str, fmt, ap);
    va_end(ap);
    return rc;
}

extern "C" int __madc_fscanf(FILE *fp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = vfscanf(fp, fmt, ap);
    va_end(ap);
    return rc;
}

extern "C" int __madc_scanf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = vscanf(fmt, ap);
    va_end(ap);
    return rc;
}

// __builtin_add/sub/mul_overflow: write a+b/a-b/a*b to *res and
// return 1 on overflow, 0 otherwise.  These use __int128 to detect
// overflow in 64-bit arithmetic.  The GCC torture tests use the
// type-generic builtins; madc aliases them to these long-width
// implementations via the lexer's define_map.
extern "C" int __madc_add_overflow(long a, long b, long *res)
{
    __int128 r = (__int128)a + (__int128)b;
    *res = (long)r;
    return r != (long)r;
}
extern "C" int __madc_sub_overflow(long a, long b, long *res)
{
    __int128 r = (__int128)a - (__int128)b;
    *res = (long)r;
    return r != (long)r;
}
extern "C" int __madc_mul_overflow(long a, long b, long *res)
{
    __int128 r = (__int128)a * (__int128)b;
    *res = (long)r;
    return r != (long)r;
}

// __builtin_bswap*: byte-swap fixed-width integer values.
extern "C" uint16_t __madc_bswap16(uint16_t x)
{
    return __builtin_bswap16(x);
}

extern "C" uint32_t __madc_bswap32(uint32_t x)
{
    return __builtin_bswap32(x);
}

extern "C" uint64_t __madc_bswap64(uint64_t x)
{
    return __builtin_bswap64(x);
}

// GCC integer bit-operation builtins.
extern "C" int __madc_ffs(unsigned int x)
{
    return __builtin_ffs((int)x);
}

extern "C" int __madc_ffsl(unsigned long x)
{
    return __builtin_ffsl((long)x);
}

extern "C" int __madc_ffsll(unsigned long long x)
{
    return __builtin_ffsll((long long)x);
}

extern "C" int __madc_clz(unsigned int x)
{
    return __builtin_clz(x);
}

extern "C" int __madc_clzl(unsigned long x)
{
    return __builtin_clzl(x);
}

extern "C" int __madc_clzll(unsigned long long x)
{
    return __builtin_clzll(x);
}

extern "C" int __madc_ctz(unsigned int x)
{
    return __builtin_ctz(x);
}

extern "C" int __madc_ctzl(unsigned long x)
{
    return __builtin_ctzl(x);
}

extern "C" int __madc_ctzll(unsigned long long x)
{
    return __builtin_ctzll(x);
}

extern "C" int __madc_clrsb(unsigned int x)
{
    return __builtin_clrsb((int)x);
}

extern "C" int __madc_clrsbl(unsigned long x)
{
    return __builtin_clrsbl((long)x);
}

extern "C" int __madc_clrsbll(unsigned long long x)
{
    return __builtin_clrsbll((long long)x);
}

extern "C" int __madc_popcount(unsigned int x)
{
    return __builtin_popcount(x);
}

extern "C" int __madc_popcountl(unsigned long x)
{
    return __builtin_popcountl(x);
}

extern "C" int __madc_popcountll(unsigned long long x)
{
    return __builtin_popcountll(x);
}

extern "C" int __madc_parity(unsigned int x)
{
    return __builtin_parity(x);
}

extern "C" int __madc_parityl(unsigned long x)
{
    return __builtin_parityl(x);
}

extern "C" int __madc_parityll(unsigned long long x)
{
    return __builtin_parityll(x);
}

// __builtin_*_overflow_p: predicate-only versions (no result pointer).
// The third argument is a type indicator — its value is ignored.
extern "C" int __madc_add_overflow_p(long a, long b, long /*type_zero*/)
{
    __int128 r = (__int128)a + (__int128)b;
    return r != (long)r;
}
extern "C" int __madc_sub_overflow_p(long a, long b, long /*type_zero*/)
{
    __int128 r = (__int128)a - (__int128)b;
    return r != (long)r;
}
extern "C" int __madc_mul_overflow_p(long a, long b, long /*type_zero*/)
{
    __int128 r = (__int128)a * (__int128)b;
    return r != (long)r;
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
