// madc embedded stdarg.h — variadic function support
//
// Matches the x86-64 System V ABI exactly as gcc/c2mir set it up (see
// c2mir's mirc_x86_64_stdarg.h): va_list is the real __va_list_tag[1] struct,
// and the ABI work is done by the compiler intrinsic __builtin_va_start.
//
// The compiler injects, at the top of every variadic function body, a local
//   va_list __va_args;  __builtin_va_start(__va_args);
// (a properly-initialized va_list for this frame). The user's
//   va_list ap; va_start(ap, last);
// copies that initialized list into `ap` (va_list is an array type, so the
// copy is element-wise, i.e. va_copy semantics — plain `ap = __va_args` is an
// illegal array assignment).

typedef struct __madc_va_list_tag {
	unsigned int gp_offset;
	unsigned int fp_offset;
	void *overflow_arg_area;
	void *reg_save_area;
} va_list[1];

#define va_start(ap, last) ((ap)[0] = (__va_args)[0])
#define va_end(ap) ((void)(ap))
#define va_copy(dest, src) ((dest)[0] = (src)[0])

// va_list is now the real ABI struct, so the v*printf family resolve to the
// real libc functions (which take a va_list) — not the old __madc_* helpers
// that unpacked a custom int64_t[] buffer.
int vsprintf(char *, const char *, va_list);
int vsnprintf(char *, unsigned long, const char *, va_list);
int vfprintf(void *, const char *, va_list);
int vprintf(const char *, va_list);
