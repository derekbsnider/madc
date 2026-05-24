// madc embedded stdarg.h — variadic function support
//
// va_list is an int64_t pointer to a packed argument buffer.
// va_start sets it to the hidden __va_args parameter injected by the compiler.
// va_arg is a compiler intrinsic (parsed specially in parseExpression).
// vsprintf/vsnprintf/vfprintf/vprintf are redirected to madc helpers that unpack the buffer.

typedef long va_list;

#define va_start(ap, last) ap = __va_args
#define va_end(ap) ((void)(ap))
#define va_copy(dest, src) dest = src

#define vsprintf __madc_vsprintf
#define vsnprintf __madc_vsnprintf
#define vfprintf __madc_vfprintf
#define vprintf __madc_vprintf
