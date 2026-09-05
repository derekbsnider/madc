/* The NATIVE half of the vector ABI interop gate: compiled by the platform's
   own compiler (gcc / aarch64-linux-gnu-gcc / Apple clang) into a shared
   object that c2m loads with -L/-l.  Every function either takes/returns
   vectors across the boundary or calls BACK into the c2m-compiled program
   through a function pointer (the interp shim under -ei, generated code
   under -eg).  Header-free apart from <stdarg.h>. */
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
