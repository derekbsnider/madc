/* The NATIVE half of the vector ABI interop gate: compiled by the platform's
   own compiler (gcc / aarch64-linux-gnu-gcc / Apple clang) into a shared
   object that c2m loads with -L/-l.  Every function either takes/returns
   vectors across the boundary or calls BACK into the c2m-compiled program
   through a function pointer (the interp shim under -ei, generated code
   under -eg).  The stack-argument PACKING probes at the end (ten ints / chars /
   shorts / floats, a mixed run, a vararg function with a named stack argument)
   are where Apple's arm64 ABI packs a non-variadic stack argument at its
   natural size while AAPCS64 and SysV give every one 8 bytes.  Header-free
   apart from <stdarg.h>. */
#include <stdarg.h>
typedef int v4si __attribute__((vector_size(16)));
typedef double v2df __attribute__((vector_size(16)));

v4si nadd(v4si a, v4si b) { return a + b; }
v4si nsum9(v4si a, v4si b, v4si c, v4si d, v4si e, v4si f, v4si g, v4si h, v4si i)
{
    return a + b + c + d + e + f + g + h + i;
}
v4si nmixed(int x, v4si a, long y, v4si b, int z)
{
    v4si k = {x, (int) y, z, x + z};
    return (a - b) + k;
}
/* seven integer args (the seventh on the x86-64 stack) then nine vectors: the
   ninth vector's stack slot must be 16-byte aligned after the 8-byte long */
v4si nstackmix(long a1, long a2, long a3, long a4, long a5, long a6, long a7,
               v4si v1, v4si v2, v4si v3, v4si v4, v4si v5, v4si v6, v4si v7, v4si v8, v4si v9)
{
    v4si k = {(int) (a1 + a2), (int) (a3 + a4), (int) (a5 + a6), (int) a7};
    return v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + k;
}
v2df ndsum(v2df a, v2df b) { return a + b; }
long long nvsum(int n, ...)
{
    va_list ap;
    long long s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
	int kind = va_arg(ap, int);
	if (kind == 0) s += va_arg(ap, int);
	else if (kind == 1) s += (long long) va_arg(ap, double);
	else if (kind == 2) { v4si v = va_arg(ap, v4si); s += v[0] + v[1] + v[2] + v[3]; }
	else { v2df d = va_arg(ap, v2df); s += (long long) (d[0] + d[1]); }
    }
    va_end(ap);
    return s;
}
v4si napply(v4si (*f)(v4si, v4si), v4si a, v4si b) { return f(a, b); }
v4si napply9(v4si (*f)(v4si, v4si, v4si, v4si, v4si, v4si, v4si, v4si, v4si), v4si a, v4si b)
{
    return f(a, a, a, a, a, a, a, a, b);
}
long long napplyva(long long (*f)(int, ...), v4si a, v2df d)
{
    return f(4, 2, a, 3, d, 0, 5, 1, 2.5);
}
double dapply9(double (*f)(double, double, double, double, double, double, double, double, double))
{
    return f(1, 2, 3, 4, 5, 6, 7, 8, 9);
}
int iapply10(int (*f)(int, int, int, int, int, int, int, int, int, int))
{
    return f(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
}
/* long double across the boundary: the eighth argument lands on the stack
   after a 8-byte long (SysV: a 16-byte aligned slot; AAPCS64 linux: a 16-byte
   Q-class value in v0..v7), and the same through `...` */
long double nld7(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long double x, long a8)
{
    return x + (long double) (a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8);
}
long double nvld(int n, int pad, ...)
{
    va_list ap;
    long double s = pad;
    int i;
    va_start(ap, pad);
    for (i = 0; i < n; i++) {
	int kind = va_arg(ap, int);
	if (kind == 0) s += va_arg(ap, long);
	else s += va_arg(ap, long double);
    }
    va_end(ap);
    return s;
}
long double ldapply(long double (*f)(long, long, long, long, long, long, long, long double, long))
{
    return f(1, 2, 3, 4, 5, 6, 7, 100.5L, 8);
}
long double vldapply(long double (*f)(int, int, ...))
{
    return f(3, 100, 0, 1L, 0, 2L, 1, 0.25L);
}
/* Stack-argument packing.  Ten arguments put the ninth and tenth on the stack
   (AAPCS64: 8-byte slots; Apple arm64: an int or float at 4 bytes, a short at
   2, a char at 1, aligned to its size); nmixpack runs a char / int / char /
   long / short / composite / char / float sequence over the packed area (the
   composite 8-byte aligned); nva9 is a vararg function whose ninth NAMED
   argument is on the stack -- its va_list starts after that argument, rounded
   up to 8.  Negative chars and shorts check the callee's sign extension. */
struct S2 { long a, b; };
int nisum10(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10)
{
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + 9 * a9 + 10 * a10;
}
int ncsum10(char a1, char a2, char a3, char a4, char a5, char a6, char a7, char a8, char a9, char a10)
{
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + 9 * a9 + 10 * a10;
}
int nssum10(short a1, short a2, short a3, short a4, short a5, short a6, short a7, short a8, short a9, short a10)
{
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + 9 * a9 + 10 * a10;
}
float nfsum10(float a1, float a2, float a3, float a4, float a5, float a6, float a7, float a8, float a9, float a10)
{
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + 9 * a9 + 10 * a10;
}
long nmixpack(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8,
              char c, int i, char c2, long l, short s, struct S2 st, char c3, float f)
{
    return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + c * 1000000L + i * 100000L + c2 * 10000L
           + l * 1000L + s * 100L + (st.a + st.b) * 10L + c3 * 3L + (long) f;
}
long nva9(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, int a9, ...)
{
    va_list ap;
    long s = a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 * 1000L;
    va_start(ap, a9);
    s += va_arg(ap, int) * 100L;
    s += va_arg(ap, long) * 10L;
    s += va_arg(ap, int);
    va_end(ap);
    return s;
}
int capply10(int (*f)(char, char, char, char, char, char, char, char, char, char))
{
    return f(1, 2, 3, 4, 5, 6, 7, 8, -9, -10);
}
int sapply10(int (*f)(short, short, short, short, short, short, short, short, short, short))
{
    return f(1, 2, 3, 4, 5, 6, 7, 8, -900, -1000);
}
float fapply10(float (*f)(float, float, float, float, float, float, float, float, float, float))
{
    return f(1, 2, 3, 4, 5, 6, 7, 8, 9.5f, 10.25f);
}
long mixapply(long (*f)(long, long, long, long, long, long, long, long,
                        char, int, char, long, short, struct S2, char, float))
{
    struct S2 st = {100, 200};
    return f(1, 2, 3, 4, 5, 6, 7, 8, -9, 10, 11, 12, -13, st, 14, 15.5f);
}
long va9apply(long (*f)(long, long, long, long, long, long, long, long, int, ...))
{
    return f(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11L, 12);
}
