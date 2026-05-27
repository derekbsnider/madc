///////////////////////////////////////////////////////////////////////////
//                                                                       //
// va_helpers.cpp — madc varargs support functions                       //
//                                                                       //
// These C functions are compiled by clang++ and linked into the madc     //
// binary.
// They bridge madc's packed int64_t[] varargs buffer to libc's printf    //
// family by parsing the format string and calling sprintf per-specifier. //
//                                                                       //
///////////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <csetjmp>
#include <type_traits>
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

// Sentinel stored in buf[0] to validate that buf[1] is a slot pointer.
// Chosen to be unlikely to appear as a random stack/heap value.
static const uintptr_t MADC_JMP_MAGIC = 0x4D41444A4D505F31ULL; // "MADJMP_1"

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
    madc_jmp_slot *slot = madc_jmp_slot_for(user_buf);
    // Store {magic, slot_ptr} in buf[0..1] so memcpy'd copies of the
    // buffer still point to the same jmp_buf.  The magic sentinel lets
    // longjmp validate before trusting buf[1] as a pointer.
    ((uintptr_t *)user_buf)[0] = MADC_JMP_MAGIC;
    ((void **)user_buf)[1] = (void *)slot;
    return (void *)slot->env;
}

extern "C" void __madc_builtin_longjmp(void *user_buf, int value)
{
    if ( value == 0 )
	value = 1;
    // Validate the magic sentinel before trusting buf[1] as a slot
    // pointer.  Falls back to map lookup if the buffer wasn't set up
    // by __madc_jmpbuf_for (or was corrupted).
    if ( ((uintptr_t *)user_buf)[0] == MADC_JMP_MAGIC )
    {
	madc_jmp_slot *slot = (madc_jmp_slot *)((void **)user_buf)[1];
	if ( slot )
	{
	    longjmp(slot->env, value);
	    return;
	}
    }
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

// __builtin_signbit: extract sign bit via movmskpd (GCC's approach).
// Returns non-zero when the sign bit is set.
extern "C" int __madc_signbit(double x)
{
    return __builtin_signbit(x);
}
extern "C" int __madc_signbitf(float x)
{
    return __builtin_signbitf(x);
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

template <typename T, typename Op>
static int madc_overflow_store(long long a, long long b, T *res, Op op)
{
    __int128 r = op((__int128)a, (__int128)b);
    *res = static_cast<T>(r);
    // For unsigned result types, widen the truncated result through the
    // unsigned type so negative infinite-precision results (which don't
    // fit in an unsigned range) are correctly detected as overflow.
    // E.g. r=-8 in __int128, T=uint64_t: *res=0xFFFFFFFFFFFFFFF8,
    // (unsigned __int128)*res = 0xFFFFFFFFFFFFFFF8 != -8 → overflow.
    if ( std::is_unsigned<T>::value )
	return (unsigned __int128)r != (unsigned __int128)(T)r;
    return r != (__int128)(T)r;
}

#define MADC_DEFINE_OVERFLOW_STORE_HELPERS(OPNAME, EXPR) \
extern "C" int __madc_##OPNAME##_overflow_s8(long long a, long long b, int8_t *res) \
{ return madc_overflow_store<int8_t>(a, b, res, []( __int128 x, __int128 y ) { return (EXPR); }); } \
extern "C" int __madc_##OPNAME##_overflow_u8(long long a, long long b, uint8_t *res) \
{ return madc_overflow_store<uint8_t>(a, b, res, []( __int128 x, __int128 y ) { return (EXPR); }); } \
extern "C" int __madc_##OPNAME##_overflow_s16(long long a, long long b, int16_t *res) \
{ return madc_overflow_store<int16_t>(a, b, res, []( __int128 x, __int128 y ) { return (EXPR); }); } \
extern "C" int __madc_##OPNAME##_overflow_u16(long long a, long long b, uint16_t *res) \
{ return madc_overflow_store<uint16_t>(a, b, res, []( __int128 x, __int128 y ) { return (EXPR); }); } \
extern "C" int __madc_##OPNAME##_overflow_s32(long long a, long long b, int32_t *res) \
{ return madc_overflow_store<int32_t>(a, b, res, []( __int128 x, __int128 y ) { return (EXPR); }); } \
extern "C" int __madc_##OPNAME##_overflow_u32(long long a, long long b, uint32_t *res) \
{ return madc_overflow_store<uint32_t>(a, b, res, []( __int128 x, __int128 y ) { return (EXPR); }); } \
extern "C" int __madc_##OPNAME##_overflow_s64(long long a, long long b, int64_t *res) \
{ return madc_overflow_store<int64_t>(a, b, res, []( __int128 x, __int128 y ) { return (EXPR); }); } \
extern "C" int __madc_##OPNAME##_overflow_u64(long long a, long long b, uint64_t *res) \
{ return madc_overflow_store<uint64_t>(a, b, res, []( __int128 x, __int128 y ) { return (EXPR); }); }

MADC_DEFINE_OVERFLOW_STORE_HELPERS(add, x + y)
MADC_DEFINE_OVERFLOW_STORE_HELPERS(sub, x - y)
MADC_DEFINE_OVERFLOW_STORE_HELPERS(mul, x * y)

#undef MADC_DEFINE_OVERFLOW_STORE_HELPERS

// Unsigned-input 64-bit overflow helpers.  When both operands are
// unsigned long (64-bit), the caller passes them as long long, which
// sign-extends 0xFFFFFFFFFFFFFFEE to -18.  These helpers reinterpret
// the inputs as unsigned before widening to unsigned __int128, so the
// mathematical value is preserved (e.g. 2^64 - 18 stays large instead
// of becoming -18).  See pr85095 vs pr91450 in the GCC torture suite.
template <typename Op>
static int madc_overflow_store_uu64(long long a, long long b, uint64_t *res, Op op)
{
    unsigned __int128 r = op(
	(unsigned __int128)(unsigned long long)a,
	(unsigned __int128)(unsigned long long)b);
    *res = (uint64_t)r;
    return r != (unsigned __int128)*res;
}

extern "C" int __madc_add_overflow_uu64(long long a, long long b, uint64_t *res)
{ return madc_overflow_store_uu64(a, b, res, [](unsigned __int128 x, unsigned __int128 y) { return x + y; }); }
extern "C" int __madc_sub_overflow_uu64(long long a, long long b, uint64_t *res)
{ return madc_overflow_store_uu64(a, b, res, [](unsigned __int128 x, unsigned __int128 y) { return x - y; }); }
extern "C" int __madc_mul_overflow_uu64(long long a, long long b, uint64_t *res)
{ return madc_overflow_store_uu64(a, b, res, [](unsigned __int128 x, unsigned __int128 y) { return x * y; }); }

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
template <typename T, typename Op>
static int madc_overflow_p_unsigned(unsigned long long a, unsigned long long b, Op op)
{
    unsigned __int128 lhs = static_cast<T>(a);
    unsigned __int128 rhs = static_cast<T>(b);
    unsigned __int128 r = op(lhs, rhs);
    return r != static_cast<T>(r);
}

template <typename T, typename Op>
static int madc_overflow_p_signed(unsigned long long a, unsigned long long b, Op op)
{
    __int128 lhs = static_cast<T>(a);
    __int128 rhs = static_cast<T>(b);
    __int128 r = op(lhs, rhs);
    return r != static_cast<T>(r);
}

#define MADC_DEFINE_OVERFLOW_P_HELPERS(OPNAME, EXPR) \
extern "C" int __madc_##OPNAME##_overflow_p_s16(unsigned long long a, unsigned long long b, unsigned long long) \
{ return madc_overflow_p_signed<short>(a, b, []( __int128 x, __int128 y ) { return (EXPR); }); } \
extern "C" int __madc_##OPNAME##_overflow_p_u16(unsigned long long a, unsigned long long b, unsigned long long) \
{ return madc_overflow_p_unsigned<unsigned short>(a, b, []( unsigned __int128 x, unsigned __int128 y ) { return (EXPR); }); } \
extern "C" int __madc_##OPNAME##_overflow_p_s32(unsigned long long a, unsigned long long b, unsigned long long) \
{ return madc_overflow_p_signed<int>(a, b, []( __int128 x, __int128 y ) { return (EXPR); }); } \
extern "C" int __madc_##OPNAME##_overflow_p_u32(unsigned long long a, unsigned long long b, unsigned long long) \
{ return madc_overflow_p_unsigned<unsigned int>(a, b, []( unsigned __int128 x, unsigned __int128 y ) { return (EXPR); }); } \
extern "C" int __madc_##OPNAME##_overflow_p_s64(unsigned long long a, unsigned long long b, unsigned long long) \
{ return madc_overflow_p_signed<long long>(a, b, []( __int128 x, __int128 y ) { return (EXPR); }); } \
extern "C" int __madc_##OPNAME##_overflow_p_u64(unsigned long long a, unsigned long long b, unsigned long long) \
{ return madc_overflow_p_unsigned<unsigned long long>(a, b, []( unsigned __int128 x, unsigned __int128 y ) { return (EXPR); }); }

MADC_DEFINE_OVERFLOW_P_HELPERS(add, x + y)
MADC_DEFINE_OVERFLOW_P_HELPERS(sub, x - y)
MADC_DEFINE_OVERFLOW_P_HELPERS(mul, x * y)

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

#undef MADC_DEFINE_OVERFLOW_P_HELPERS

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

// -----------------------------------------------------------------------
// Compiler builtin wrappers — expose clang++ builtins as extern "C"
// so transpiled code (c2mir) can call them via dlsym.
// -----------------------------------------------------------------------

// __builtin_object_size: compile-time only — runtime always returns -1 (unknown)
extern "C" unsigned long __madc_builtin_object_size(void *ptr, int mode)
{
    (void)ptr; (void)mode;
    return (unsigned long)-1;
}

// __builtin___strcpy_chk family — bounds-checked string operations
extern "C" char *__madc_builtin_strcpy_chk(char *dst, const char *src, unsigned long size)
{
    return __builtin___strcpy_chk(dst, src, size);
}

extern "C" char *__madc_builtin_stpcpy_chk(char *dst, const char *src, unsigned long size)
{
    return __builtin___stpcpy_chk(dst, src, size);
}

extern "C" char *__madc_builtin_stpncpy_chk(char *dst, const char *src, unsigned long n, unsigned long size)
{
    return __builtin___stpncpy_chk(dst, src, n, size);
}

extern "C" char *__madc_builtin_strcat_chk(char *dst, const char *src, unsigned long size)
{
    return __builtin___strcat_chk(dst, src, size);
}

// __builtin_frame_address: return caller's frame pointer
extern "C" void *__madc_builtin_frame_address(int level)
{
    // level must be a compile-time constant for the real builtin;
    // we only support level 0 (current frame).
    return __builtin_frame_address(0);
}

// __builtin_setjmp / __builtin_longjmp — map to standard setjmp/longjmp
#include <setjmp.h>
extern "C" int __madc_builtin_setjmp(void *buf)
{
    return setjmp(*(jmp_buf *)buf);
}

extern "C" void __madc_builtin_longjmp_val(void *buf, int val)
{
    longjmp(*(jmp_buf *)buf, val);
}

// __builtin_uabs / __builtin_umaxabs — unsigned absolute value
#include <stdint.h>
extern "C" unsigned int __madc_builtin_uabs(int x)
{
    return (x < 0) ? (unsigned int)(-(long long)x) : (unsigned int)x;
}

extern "C" uintmax_t __madc_builtin_umaxabs(intmax_t x)
{
    return (x < 0) ? (uintmax_t)(-((long long)x + 1)) + 1 : (uintmax_t)x;
}

// -----------------------------------------------------------------------
// _Complex lowering — struct-based complex arithmetic helpers.
// Each type gets a struct { T re; T im; } and a full set of operations.
// Mirrors the legacy DataDefCOMPLEX layout exactly.
// -----------------------------------------------------------------------

// Struct types (must match emitter preamble typedefs)
typedef struct { double re; double im; } __madc_cdouble;
typedef struct { float re; float im; } __madc_cfloat;
typedef struct { int re; int im; } __madc_cint;
typedef struct { unsigned int re; unsigned int im; } __madc_cuint;
typedef struct { unsigned short re; unsigned short im; } __madc_cushort;
typedef struct { long re; long im; } __madc_clong;
typedef struct { unsigned long re; unsigned long im; } __madc_culong;

// Macro to generate all operations for a complex type.
// T = scalar type, N = name suffix (e.g. cdouble, cfloat)
#define MADC_COMPLEX_OPS(T, N) \
extern "C" __madc_##N __madc_##N##_add(__madc_##N a, __madc_##N b) \
    { return (__madc_##N){a.re+b.re, a.im+b.im}; } \
extern "C" __madc_##N __madc_##N##_sub(__madc_##N a, __madc_##N b) \
    { return (__madc_##N){a.re-b.re, a.im-b.im}; } \
extern "C" __madc_##N __madc_##N##_mul(__madc_##N a, __madc_##N b) \
    { return (__madc_##N){a.re*b.re - a.im*b.im, a.re*b.im + a.im*b.re}; } \
extern "C" __madc_##N __madc_##N##_div(__madc_##N a, __madc_##N b) \
    { T denom = b.re*b.re + b.im*b.im; \
      return (__madc_##N){(a.re*b.re + a.im*b.im)/denom, \
                          (a.im*b.re - a.re*b.im)/denom}; } \
extern "C" __madc_##N __madc_##N##_conj(__madc_##N a) \
    { return (__madc_##N){a.re, -a.im}; } \
extern "C" __madc_##N __madc_##N##_neg(__madc_##N a) \
    { return (__madc_##N){-a.re, -a.im}; } \
extern "C" int __madc_##N##_eq(__madc_##N a, __madc_##N b) \
    { return a.re == b.re && a.im == b.im; } \
extern "C" int __madc_##N##_ne(__madc_##N a, __madc_##N b) \
    { return a.re != b.re || a.im != b.im; } \
extern "C" __madc_##N __madc_##N##_make(T re, T im) \
    { return (__madc_##N){re, im}; } \
extern "C" __madc_##N __madc_##N##_from_real(T re) \
    { return (__madc_##N){re, 0}; } \
extern "C" T __madc_##N##_real(__madc_##N a) { return a.re; } \
extern "C" T __madc_##N##_imag(__madc_##N a) { return a.im; }

MADC_COMPLEX_OPS(double, cdouble)
MADC_COMPLEX_OPS(float, cfloat)
MADC_COMPLEX_OPS(int, cint)
MADC_COMPLEX_OPS(unsigned int, cuint)
MADC_COMPLEX_OPS(unsigned short, cushort)
MADC_COMPLEX_OPS(long, clong)
MADC_COMPLEX_OPS(unsigned long, culong)
