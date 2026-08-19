/* libc_signatures.cpp — the C library's return types, and madc's own runtime
 * helpers'. See include/libc_signatures.h for why this is one owner.
 *
 * THE DEFAULT IS NOT IN THIS TABLE. An unlisted name returns LibcRet::Unknown
 * and the caller declares it `int`, which is what C says an implicit declaration
 * returns. So this table holds only the EXCEPTIONS to `int` — plus the
 * int-returning families that the `__builtin_` alias map rewrites to, which are
 * listed explicitly so the alias gate can require an entry for every target.
 *
 * A missed POINTER returner is loud: `int f()` assigned to a pointer is c2mir's
 * "using integer without cast for pointer type" warning and the zero-warnings
 * law fails the build on it. A missed `long` / `size_t` returner is the residual
 * silent risk, which is why those classes are enumerated deliberately rather
 * than left to the default.
 */

#include "libc_signatures.h"

#include <map>

namespace {

struct Entry {
	const char *name;
	LibcRet ret;
};

/* The C99 <math.h> roots. THE list — src/lexer.cpp expands it into
 * `__builtin_<root>{,f,l}` aliases and math_class_of() below expands the same
 * list into return classes. Two expansions of one list cannot disagree about
 * which names exist; two hand-maintained lists did, for fifty-one of these. */
const char *const math_roots[] = {
	"acos", "acosh", "asin", "asinh", "atan", "atan2", "atanh",
	"cbrt", "ceil", "copysign", "cos", "cosh", "erf", "erfc",
	"exp", "exp2", "expm1", "fabs", "fdim", "floor", "fma",
	"fmax", "fmin", "fmod", "frexp", "hypot", "ilogb", "ldexp",
	"lgamma", "llrint", "llround", "log", "log10", "log1p",
	"log2", "logb", "lrint", "lround", "modf", "nearbyint",
	"nextafter", "nexttoward", "pow", "remainder", "remquo",
	"rint", "round", "scalbln", "scalbn", "sin", "sinh", "sqrt",
	"tan", "tanh", "tgamma", "trunc", NULL
};

/* Five roots are not part of the real family: their whole point is to return an
 * integer. The suffix still selects nothing — `lroundf` returns `long`, not
 * `float` — so they carry one class across all three spellings. */
const Entry math_integer_roots[] = {
	{ "ilogb",   LibcRet::Int   },
	{ "lrint",   LibcRet::Long  },
	{ "lround",  LibcRet::Long  },
	{ "llrint",  LibcRet::Int64 },
	{ "llround", LibcRet::Int64 },
	{ NULL, LibcRet::Unknown }
};

/* The argument shapes. A root not listed here takes shape (T) — the
 * 1-argument majority. `nexttoward`'s second parameter is `long double` at every
 * suffix (C99: `float nexttowardf(float, long double)`), which is why it is its
 * own shape rather than T_T. */
struct ShapeEntry {
	const char *root;
	LibcArgs args;
};

const ShapeEntry math_shapes[] = {
	{ "atan2", LibcArgs::T_T },	{ "copysign", LibcArgs::T_T },
	{ "fdim", LibcArgs::T_T },	{ "fmax", LibcArgs::T_T },
	{ "fmin", LibcArgs::T_T },	{ "fmod", LibcArgs::T_T },
	{ "hypot", LibcArgs::T_T },	{ "nextafter", LibcArgs::T_T },
	{ "pow", LibcArgs::T_T },	{ "remainder", LibcArgs::T_T },
	{ "fma", LibcArgs::T_T_T },
	{ "ldexp", LibcArgs::T_Int },	{ "scalbn", LibcArgs::T_Int },
	{ "scalbln", LibcArgs::T_Long },
	{ "modf", LibcArgs::T_Tptr },
	{ "frexp", LibcArgs::T_Intptr },
	{ "remquo", LibcArgs::T_T_Intptr },
	{ "nexttoward", LibcArgs::T_LDouble },
	{ NULL, LibcArgs::None }
};

const Entry signatures[] = {
	/* --- void ------------------------------------------------------- */
	/* A `long` prototype here types the assert idiom's ternary branch
	 * `(e) ? (void)0 : (printf(...), abort())` as `long`, which c2mir
	 * rejects outright — the one class in this file that was already
	 * partly enumerated. */
	{ "abort", LibcRet::Void }, { "exit", LibcRet::Void },
	{ "_exit", LibcRet::Void }, { "_Exit", LibcRet::Void },
	{ "quick_exit", LibcRet::Void },
	{ "free", LibcRet::Void }, { "cfree", LibcRet::Void },
	{ "perror", LibcRet::Void },
	{ "srand", LibcRet::Void }, { "srandom", LibcRet::Void },
	{ "srand48", LibcRet::Void }, { "lcong48", LibcRet::Void },
	{ "__assert_fail", LibcRet::Void }, { "__assert", LibcRet::Void },
	{ "__assert_perror_fail", LibcRet::Void },
	{ "qsort", LibcRet::Void }, { "qsort_r", LibcRet::Void },
	{ "bzero", LibcRet::Void }, { "bcopy", LibcRet::Void },
	{ "swab", LibcRet::Void },
	{ "rewind", LibcRet::Void }, { "clearerr", LibcRet::Void },
	{ "clearerr_unlocked", LibcRet::Void },
	{ "setbuf", LibcRet::Void }, { "setbuffer", LibcRet::Void },
	{ "setlinebuf", LibcRet::Void },
	{ "flockfile", LibcRet::Void }, { "funlockfile", LibcRet::Void },
	{ "longjmp", LibcRet::Void }, { "_longjmp", LibcRet::Void },
	{ "siglongjmp", LibcRet::Void },
	{ "tzset", LibcRet::Void },
	{ "openlog", LibcRet::Void }, { "closelog", LibcRet::Void },
	{ "syslog", LibcRet::Void }, { "vsyslog", LibcRet::Void },
	{ "freeaddrinfo", LibcRet::Void },
	{ "endpwent", LibcRet::Void }, { "endgrent", LibcRet::Void },
	{ "endhostent", LibcRet::Void }, { "endservent", LibcRet::Void },
	{ "rewinddir", LibcRet::Void }, { "seekdir", LibcRet::Void },
	{ "pthread_exit", LibcRet::Void },

	/* --- char * ----------------------------------------------------- */
	{ "asctime", LibcRet::CharPtr }, { "asctime_r", LibcRet::CharPtr },
	{ "ctime", LibcRet::CharPtr }, { "ctime_r", LibcRet::CharPtr },
	{ "strcpy", LibcRet::CharPtr }, { "strncpy", LibcRet::CharPtr },
	{ "strcat", LibcRet::CharPtr }, { "strncat", LibcRet::CharPtr },
	{ "strchr", LibcRet::CharPtr }, { "strrchr", LibcRet::CharPtr },
	{ "strchrnul", LibcRet::CharPtr },
	{ "strstr", LibcRet::CharPtr }, { "strcasestr", LibcRet::CharPtr },
	{ "strpbrk", LibcRet::CharPtr },
	{ "strtok", LibcRet::CharPtr }, { "strtok_r", LibcRet::CharPtr },
	{ "strsep", LibcRet::CharPtr },
	{ "strdup", LibcRet::CharPtr }, { "strndup", LibcRet::CharPtr },
	{ "strerror", LibcRet::CharPtr }, { "strerror_r", LibcRet::CharPtr },
	{ "strsignal", LibcRet::CharPtr },
	{ "index", LibcRet::CharPtr }, { "rindex", LibcRet::CharPtr },
	{ "stpcpy", LibcRet::CharPtr }, { "stpncpy", LibcRet::CharPtr },
	{ "getenv", LibcRet::CharPtr }, { "secure_getenv", LibcRet::CharPtr },
	{ "gets", LibcRet::CharPtr },
	{ "fgets", LibcRet::CharPtr }, { "fgets_unlocked", LibcRet::CharPtr },
	{ "basename", LibcRet::CharPtr }, { "dirname", LibcRet::CharPtr },
	{ "setlocale", LibcRet::CharPtr }, { "nl_langinfo", LibcRet::CharPtr },
	{ "tmpnam", LibcRet::CharPtr }, { "tempnam", LibcRet::CharPtr },
	{ "mktemp", LibcRet::CharPtr }, { "mkdtemp", LibcRet::CharPtr },
	{ "ctermid", LibcRet::CharPtr }, { "ttyname", LibcRet::CharPtr },
	{ "getcwd", LibcRet::CharPtr }, { "getwd", LibcRet::CharPtr },
	{ "get_current_dir_name", LibcRet::CharPtr },
	{ "realpath", LibcRet::CharPtr }, { "crypt", LibcRet::CharPtr },
	{ "inet_ntoa", LibcRet::CharPtr }, { "strptime", LibcRet::CharPtr },

	/* --- void * (and every other 8-byte pointer: FILE *, DIR *,
	 *     struct tm *, a signal handler) --------------------------- */
	{ "malloc", LibcRet::VoidPtr }, { "calloc", LibcRet::VoidPtr },
	{ "realloc", LibcRet::VoidPtr }, { "reallocarray", LibcRet::VoidPtr },
	{ "aligned_alloc", LibcRet::VoidPtr }, { "valloc", LibcRet::VoidPtr },
	{ "memalign", LibcRet::VoidPtr }, { "pvalloc", LibcRet::VoidPtr },
	{ "alloca", LibcRet::VoidPtr },
	{ "memcpy", LibcRet::VoidPtr }, { "memmove", LibcRet::VoidPtr },
	{ "memset", LibcRet::VoidPtr }, { "mempcpy", LibcRet::VoidPtr },
	{ "memchr", LibcRet::VoidPtr }, { "memrchr", LibcRet::VoidPtr },
	{ "rawmemchr", LibcRet::VoidPtr }, { "memmem", LibcRet::VoidPtr },
	{ "memccpy", LibcRet::VoidPtr },
	{ "bsearch", LibcRet::VoidPtr },
	{ "mmap", LibcRet::VoidPtr }, { "mmap64", LibcRet::VoidPtr },
	{ "mremap", LibcRet::VoidPtr }, { "sbrk", LibcRet::VoidPtr },
	{ "shmat", LibcRet::VoidPtr },
	{ "dlopen", LibcRet::VoidPtr }, { "dlsym", LibcRet::VoidPtr },
	{ "dlvsym", LibcRet::VoidPtr }, { "dlmopen", LibcRet::VoidPtr },
	{ "dlerror", LibcRet::CharPtr },
	{ "fopen", LibcRet::VoidPtr }, { "fopen64", LibcRet::VoidPtr },
	{ "fdopen", LibcRet::VoidPtr }, { "freopen", LibcRet::VoidPtr },
	{ "popen", LibcRet::VoidPtr }, { "tmpfile", LibcRet::VoidPtr },
	{ "opendir", LibcRet::VoidPtr }, { "fdopendir", LibcRet::VoidPtr },
	{ "readdir", LibcRet::VoidPtr }, { "readdir64", LibcRet::VoidPtr },
	{ "localtime", LibcRet::VoidPtr }, { "localtime_r", LibcRet::VoidPtr },
	{ "gmtime", LibcRet::VoidPtr }, { "gmtime_r", LibcRet::VoidPtr },
	{ "signal", LibcRet::VoidPtr }, { "bsd_signal", LibcRet::VoidPtr },
	{ "sigset", LibcRet::VoidPtr },
	{ "lfind", LibcRet::VoidPtr }, { "lsearch", LibcRet::VoidPtr },
	{ "tsearch", LibcRet::VoidPtr }, { "tfind", LibcRet::VoidPtr },
	{ "tdelete", LibcRet::VoidPtr },
	{ "iconv_open", LibcRet::VoidPtr },
	{ "getpwnam", LibcRet::VoidPtr }, { "getpwuid", LibcRet::VoidPtr },
	{ "getgrnam", LibcRet::VoidPtr }, { "getgrgid", LibcRet::VoidPtr },
	{ "gethostbyname", LibcRet::VoidPtr },
	{ "gethostbyaddr", LibcRet::VoidPtr },
	{ "pthread_getspecific", LibcRet::VoidPtr },
	/* locale_t IS a pointer. libstdc++'s bits/c++locale.h declares
	 * `extern "C" __typeof(uselocale) __uselocale;` — a typeof declaration
	 * madc cannot resolve (C11 has no typeof), so the call reaches this
	 * fallback, and an integer passed to __uselocale's own pointer parameter
	 * is c2mir's "using integer without cast for pointer type parameter".
	 * That warning is how a missed pointer-returner announces itself. */
	{ "newlocale", LibcRet::VoidPtr }, { "__newlocale", LibcRet::VoidPtr },
	{ "duplocale", LibcRet::VoidPtr }, { "__duplocale", LibcRet::VoidPtr },
	{ "uselocale", LibcRet::VoidPtr }, { "__uselocale", LibcRet::VoidPtr },
	{ "freelocale", LibcRet::Void }, { "__freelocale", LibcRet::Void },
	{ "nl_langinfo_l", LibcRet::CharPtr },

	/* --- size_t and the always-64 unsigned returns ------------------ */
	{ "strlen", LibcRet::UInt64 }, { "strnlen", LibcRet::UInt64 },
	{ "strspn", LibcRet::UInt64 }, { "strcspn", LibcRet::UInt64 },
	{ "strxfrm", LibcRet::UInt64 },
	{ "strlcpy", LibcRet::UInt64 }, { "strlcat", LibcRet::UInt64 },
	{ "fread", LibcRet::UInt64 }, { "fwrite", LibcRet::UInt64 },
	{ "fread_unlocked", LibcRet::UInt64 },
	{ "fwrite_unlocked", LibcRet::UInt64 },
	{ "mbstowcs", LibcRet::UInt64 }, { "wcstombs", LibcRet::UInt64 },
	{ "wcslen", LibcRet::UInt64 }, { "wcsnlen", LibcRet::UInt64 },
	{ "strftime", LibcRet::UInt64 }, { "wcsftime", LibcRet::UInt64 },
	{ "confstr", LibcRet::UInt64 }, { "iconv", LibcRet::UInt64 },
	{ "malloc_usable_size", LibcRet::UInt64 },
	{ "__ctype_get_mb_cur_max", LibcRet::UInt64 },
	{ "mbrlen", LibcRet::UInt64 }, { "mbrtowc", LibcRet::UInt64 },
	{ "wcrtomb", LibcRet::UInt64 }, { "mbsrtowcs", LibcRet::UInt64 },
	{ "wcsrtombs", LibcRet::UInt64 },
	{ "strtoull", LibcRet::UInt64 }, { "strtoumax", LibcRet::UInt64 },

	/* --- long long / time_t / off_t (always 64) ---------------------- */
	{ "llabs", LibcRet::Int64 }, { "imaxabs", LibcRet::Int64 },
	{ "atoll", LibcRet::Int64 }, { "strtoll", LibcRet::Int64 },
	{ "strtoimax", LibcRet::Int64 },
	{ "time", LibcRet::Int64 }, { "mktime", LibcRet::Int64 },
	{ "timegm", LibcRet::Int64 },
	{ "lseek", LibcRet::Int64 }, { "lseek64", LibcRet::Int64 },
	{ "ftello", LibcRet::Int64 }, { "ftello64", LibcRet::Int64 },

	/* --- platform long (4 bytes on LLP64) --------------------------- */
	/* ssize_t lives here: read/write returning `int` would report a
	 * >2GB transfer, and -1 is the error path every caller tests. */
	{ "labs", LibcRet::Long }, { "atol", LibcRet::Long },
	{ "strtol", LibcRet::Long },
	{ "ftell", LibcRet::Long }, { "telldir", LibcRet::Long },
	{ "sysconf", LibcRet::Long }, { "pathconf", LibcRet::Long },
	{ "fpathconf", LibcRet::Long }, { "gethostid", LibcRet::Long },
	{ "random", LibcRet::Long }, { "a64l", LibcRet::Long },
	{ "lrand48", LibcRet::Long }, { "mrand48", LibcRet::Long },
	{ "jrand48", LibcRet::Long }, { "nrand48", LibcRet::Long },
	{ "clock", LibcRet::Long }, { "times", LibcRet::Long },
	{ "ulimit", LibcRet::Long },
	{ "read", LibcRet::Long }, { "write", LibcRet::Long },
	{ "pread", LibcRet::Long }, { "pwrite", LibcRet::Long },
	{ "readlink", LibcRet::Long }, { "readlinkat", LibcRet::Long },
	{ "send", LibcRet::Long }, { "recv", LibcRet::Long },
	{ "sendto", LibcRet::Long }, { "recvfrom", LibcRet::Long },
	{ "sendmsg", LibcRet::Long }, { "recvmsg", LibcRet::Long },
	{ "getline", LibcRet::Long }, { "getdelim", LibcRet::Long },
	{ "strtoul", LibcRet::ULong },
	{ "umask", LibcRet::UInt32 },

	/* --- double / float / long double ------------------------------- */
	/* The math families come from math_roots above; these are the real
	 * returns that are not <math.h> functions. */
	{ "atof", LibcRet::Double }, { "strtod", LibcRet::Double },
	{ "difftime", LibcRet::Double },
	{ "drand48", LibcRet::Double }, { "erand48", LibcRet::Double },
	{ "strtof", LibcRet::Float }, { "strtold", LibcRet::LDouble },
	/* The locale-explicit conversion family, both spellings — libstdc++
	 * calls the __-prefixed ones directly. */
	{ "strtod_l", LibcRet::Double }, { "__strtod_l", LibcRet::Double },
	{ "strtof_l", LibcRet::Float }, { "__strtof_l", LibcRet::Float },
	{ "strtold_l", LibcRet::LDouble }, { "__strtold_l", LibcRet::LDouble },
	{ "strtol_l", LibcRet::Long }, { "__strtol_l", LibcRet::Long },
	{ "strtoul_l", LibcRet::ULong }, { "__strtoul_l", LibcRet::ULong },
	{ "strtoll_l", LibcRet::Int64 }, { "__strtoll_l", LibcRet::Int64 },
	{ "strtoull_l", LibcRet::UInt64 }, { "__strtoull_l", LibcRet::UInt64 },
	{ "strxfrm_l", LibcRet::UInt64 }, { "__strxfrm_l", LibcRet::UInt64 },
	{ "strftime_l", LibcRet::UInt64 },

	/* --- explicitly int -------------------------------------------- */
	/* Same class as the default, listed anyway: every one of these is an
	 * alias target, and the alias gate requires an entry for each. They
	 * are also the family the defect was found in — `strcmp(a,b) < 0`
	 * evaluated FALSE for a whole release. */
	{ "memcmp", LibcRet::Int }, { "strcmp", LibcRet::Int },
	{ "strncmp", LibcRet::Int }, { "strcoll", LibcRet::Int },
	{ "strcasecmp", LibcRet::Int }, { "strncasecmp", LibcRet::Int },
	{ "strverscmp", LibcRet::Int },
	{ "printf", LibcRet::Int }, { "fprintf", LibcRet::Int },
	{ "sprintf", LibcRet::Int }, { "snprintf", LibcRet::Int },
	{ "vprintf", LibcRet::Int }, { "vfprintf", LibcRet::Int },
	{ "vsprintf", LibcRet::Int }, { "vsnprintf", LibcRet::Int },
	{ "dprintf", LibcRet::Int }, { "vdprintf", LibcRet::Int },
	{ "printf_unlocked", LibcRet::Int },
	{ "fprintf_unlocked", LibcRet::Int },
	{ "scanf", LibcRet::Int }, { "fscanf", LibcRet::Int },
	{ "sscanf", LibcRet::Int }, { "vscanf", LibcRet::Int },
	{ "vfscanf", LibcRet::Int }, { "vsscanf", LibcRet::Int },
	{ "puts", LibcRet::Int }, { "putchar", LibcRet::Int },
	{ "fputc", LibcRet::Int }, { "fputs", LibcRet::Int },
	{ "fputs_unlocked", LibcRet::Int },
	{ "fputc_unlocked", LibcRet::Int },
	{ "abs", LibcRet::Int }, { "atoi", LibcRet::Int },
	{ "rand", LibcRet::Int },

	/* --- madc's own runtime helpers the alias map rewrites TO -------- */
	/* Same route as a libc name: an alias target with no declaration in
	 * scope. `__builtin_bswap64` was right only because the old default
	 * happened to be 64-bit wide. */
	{ "__madc_bswap16", LibcRet::UInt16 },
	{ "__madc_bswap32", LibcRet::UInt32 },
	{ "__madc_bswap64", LibcRet::UInt64 },
	{ "__madc_builtin_object_size", LibcRet::ULong },
	{ "__madc_builtin_uabs", LibcRet::UInt32 },
	{ "__madc_builtin_umaxabs", LibcRet::UInt64 },
	{ "__madc_builtin_frame_address", LibcRet::VoidPtr },
	{ "__madc_builtin_longjmp", LibcRet::Void },
	{ "__madc_builtin_longjmp_val", LibcRet::Void },
	{ "__madc_builtin_strcpy_chk", LibcRet::CharPtr },
	{ "__madc_builtin_stpcpy_chk", LibcRet::CharPtr },
	{ "__madc_builtin_stpncpy_chk", LibcRet::CharPtr },
	{ "__madc_builtin_strcat_chk", LibcRet::CharPtr },
	{ "__madc_builtin_strncpy_chk", LibcRet::CharPtr },
	{ "__madc_builtin_strncat_chk", LibcRet::CharPtr },
	{ "__madc_builtin_memcpy_chk", LibcRet::VoidPtr },
	{ "__madc_builtin_memmove_chk", LibcRet::VoidPtr },
	{ "__madc_builtin_mempcpy_chk", LibcRet::VoidPtr },
	{ "__madc_builtin_memset_chk", LibcRet::VoidPtr },
	{ "__madc_jmpbuf_for", LibcRet::VoidPtr },
	{ "__madc_istream_extract", LibcRet::VoidPtr },
	{ "__madc_atomic_fetch_add_l", LibcRet::Int64 },
	{ "__madc_atomic_thread_fence", LibcRet::Void },
	{ "__madc_atomic_signal_fence", LibcRet::Void },
	{ "__madc_fd_zero", LibcRet::Void },
	{ "__madc_fd_set", LibcRet::Void },
	{ "__madc_fd_clr", LibcRet::Void },
	{ "__madc_fd_isset", LibcRet::Long },
	{ "__madc_timeval_sec", LibcRet::Long },
	{ "__madc_timeval_usec", LibcRet::Long },
	{ "__madc_timerisset", LibcRet::Long },
	{ "__madc_timerclear", LibcRet::Void },
	{ "__madc_timeradd", LibcRet::Void },
	{ "__madc_timersub", LibcRet::Void },
	/* gcc's bit-manipulation builtins all return `int`, at every width —
	 * __builtin_clzll(x) is an int even though x is 64-bit. Listed rather
	 * than left to the default so the alias gate stays total: a target with
	 * no entry is how the whole class went unchecked. */
	{ "__madc_clz", LibcRet::Int }, { "__madc_clzl", LibcRet::Int },
	{ "__madc_clzll", LibcRet::Int },
	{ "__madc_ctz", LibcRet::Int }, { "__madc_ctzl", LibcRet::Int },
	{ "__madc_ctzll", LibcRet::Int },
	{ "__madc_ffs", LibcRet::Int }, { "__madc_ffsl", LibcRet::Int },
	{ "__madc_ffsll", LibcRet::Int },
	{ "__madc_clrsb", LibcRet::Int }, { "__madc_clrsbl", LibcRet::Int },
	{ "__madc_clrsbll", LibcRet::Int },
	{ "__madc_popcount", LibcRet::Int },
	{ "__madc_popcountl", LibcRet::Int },
	{ "__madc_popcountll", LibcRet::Int },
	{ "__madc_parity", LibcRet::Int },
	{ "__madc_parityl", LibcRet::Int },
	{ "__madc_parityll", LibcRet::Int },
	{ "__madc_add_overflow_p", LibcRet::Int },
	{ "__madc_sub_overflow_p", LibcRet::Int },
	{ "__madc_mul_overflow_p", LibcRet::Int },
	{ "__madc_builtin_setjmp", LibcRet::Int },

	{ NULL, LibcRet::Unknown }
};

typedef std::map<std::string, LibcRet> table_t;

/* Built on FIRST QUERY, not at static-init: an undeclared libc call is not on
 * every program's path, and startup latency is a tracked budget. */
const table_t &table(void)
{
	static table_t m;
	if ( m.empty() )
	{
		for ( int i = 0; signatures[i].name; ++i )
			m[signatures[i].name] = signatures[i].ret;
	}
	return m;
}

/* Resolve `name` as a member of a <math.h> family: the ROOT it belongs to and
 * the real type its suffix names. ONE owner of the suffix rule, because both the
 * return class and the argument shape have to agree about which family a name is
 * in — `lroundf` takes a float and returns a long, and a second copy of this
 * resolution is how the two would come to disagree.
 *
 * The name is tried AS a root first: `modf` and `erf` end in 'f' and are roots
 * themselves. */
bool math_family_of(const std::string &name, std::string *root_out,
		    LibcRet *family_out)
{
	if ( name.empty() )
		return false;

	for ( int i = 0; math_roots[i]; ++i )
	{
		if ( name != math_roots[i] )
			continue;
		if ( root_out )   *root_out = name;
		if ( family_out ) *family_out = LibcRet::Double;
		return true;
	}

	char suffix = name[name.size() - 1];
	if ( suffix != 'f' && suffix != 'l' )
		return false;
	std::string root = name.substr(0, name.size() - 1);
	for ( int i = 0; math_roots[i]; ++i )
	{
		if ( root != math_roots[i] )
			continue;
		if ( root_out )   *root_out = root;
		if ( family_out )
			*family_out = suffix == 'f' ? LibcRet::Float
						    : LibcRet::LDouble;
		return true;
	}
	return false;
}

/* The return class of a family member. The suffix picks the width for the real
 * families and picks NOTHING for the five integer roots — `lroundf` returns
 * `long`, and reading it as a float is the same class of bug this file closes. */
LibcRet math_class_of(const std::string &name)
{
	std::string root;
	LibcRet family = LibcRet::Unknown;
	if ( !math_family_of(name, &root, &family) )
		return LibcRet::Unknown;
	for ( int i = 0; math_integer_roots[i].name; ++i )
		if ( root == math_integer_roots[i].name )
			return math_integer_roots[i].ret;
	return family;
}

} // namespace

LibcRet madc_libc_return_class(const std::string &name)
{
	const table_t &m = table();
	table_t::const_iterator it = m.find(name);
	if ( it != m.end() )
		return it->second;
	return math_class_of(name);
}

const char *const *madc_libc_math_roots(void)
{
	return math_roots;
}

LibcArgs madc_libc_arg_shape(const std::string &name, LibcRet *family)
{
	std::string root;
	LibcRet fam = LibcRet::Unknown;
	if ( !math_family_of(name, &root, &fam) )
		return LibcArgs::None;
	if ( family )
		*family = fam;
	for ( int i = 0; math_shapes[i].root; ++i )
		if ( root == math_shapes[i].root )
			return math_shapes[i].args;
	return LibcArgs::T;
}
