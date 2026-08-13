// madc embedded stdarg.h — variadic function support
//
// Matches the x86-64 System V ABI exactly as gcc/c2mir set it up (see
// c2mir's mirc_x86_64_stdarg.h): va_list is the real __va_list_tag[1] struct,
// and the ABI work is done by the compiler intrinsic __builtin_va_start.
//
// The user's
//   va_list ap; va_start(ap, last);
// lowers directly to c2mir's intrinsic __builtin_va_start(ap), which takes the
// va_list alone and derives the named-argument count from the enclosing
// function's signature — exactly c2mir's own mirc_x86_64_stdarg.h idiom.
// (An earlier model primed a hidden function-entry `__va_args` master and
// copied it per use site — `(ap)[0] = (__va_args)[0]` — but that extra
// va_list struct copy left reg_save_area mis-set in large/complex frames,
// corrupting the list; invoking the intrinsic on the user's own va_list
// avoids the copy and the corruption.)

// glibc's __need protocol, exactly like real gcc stdarg.h: a header asking
// only for __gnuc_va_list (`#define __need___va_list` + include) gets that
// one typedef and the request macro is CLEARED — leaving it defined would
// mark every later include in the TU as a protocol visit
// (need_protocol_macro_live) and defeat the once-only caches TU-wide.
#ifdef __need___va_list
#undef __need___va_list
typedef __builtin_va_list __gnuc_va_list;
#else

// ONE definition: the compiler owns the type (Program::builtin_va_list_type —
// the SysV struct __madc_va_list_tag[1] singleton, resolved from the spelling
// __builtin_va_list). This header only aliases it, exactly like real gcc
// stdarg.h; the CIR emitter synthesizes the struct+typedef C when a module
// references the type.
typedef __builtin_va_list va_list;

// GCC's name for the same underlying va_list ABI type. Real system headers
// (e.g. glibc <wchar.h> under `#define __need___va_list`) reference it and then
// do `typedef __gnuc_va_list va_list;`.
typedef __builtin_va_list __gnuc_va_list;

#define va_start(ap, last) __builtin_va_start(ap)
#define va_end(ap) ((void)(ap))
// va_copy follows the target's va_list shape (Program::builtin_va_list_type):
// SysV array-of-struct copies the one element; win64's scalar `char *`
// (mingw/MSVC vadefs.h) is a plain assignment.
#ifdef _WIN32
#define va_copy(dest, src) ((dest) = (src))
#else
#define va_copy(dest, src) ((dest)[0] = (src)[0])
#endif

// va_list is now the real ABI struct, so the v*printf family resolve to the
// real libc functions (which take a va_list) — not the old __madc_* helpers
// that unpacked a custom int64_t[] buffer.
int vsprintf(char *, const char *, va_list);
int vsnprintf(char *, unsigned long, const char *, va_list);
int vfprintf(void *, const char *, va_list);
int vprintf(const char *, va_list);

#endif /* __need___va_list */
