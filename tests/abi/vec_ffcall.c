/* The c2m half of the vector ABI interop gate (see libvecnative.c): calls the
   native functions with vectors (declared, nine-deep, mixed with integers,
   varargs) and hands them callbacks that receive vectors, nine vectors, a
   vector vararg list, nine doubles and ten ints.  Oracle: the same file
   compiled by the platform compiler and linked with libvecnative.c. */
int printf(const char *, ...);
#include <stdarg.h>
typedef int v4si __attribute__((vector_size(16)));
typedef double v2df __attribute__((vector_size(16)));

v4si nadd(v4si a, v4si b);
v4si nsum9(v4si a, v4si b, v4si c, v4si d, v4si e, v4si f, v4si g, v4si h, v4si i);
v4si nmixed(int x, v4si a, long y, v4si b, int z);
v4si nstackmix(long a1, long a2, long a3, long a4, long a5, long a6, long a7,
               v4si v1, v4si v2, v4si v3, v4si v4, v4si v5, v4si v6, v4si v7, v4si v8, v4si v9);
v2df ndsum(v2df a, v2df b);
long long nvsum(int n, ...);
v4si napply(v4si (*f)(v4si, v4si), v4si a, v4si b);
v4si napply9(v4si (*f)(v4si, v4si, v4si, v4si, v4si, v4si, v4si, v4si, v4si), v4si a, v4si b);
long long napplyva(long long (*f)(int, ...), v4si a, v2df d);
double dapply9(double (*f)(double, double, double, double, double, double, double, double, double));
int iapply10(int (*f)(int, int, int, int, int, int, int, int, int, int));
long double nld7(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long double x, long a8);
long double nvld(int n, int pad, ...);
long double ldapply(long double (*f)(long, long, long, long, long, long, long, long double, long));
long double vldapply(long double (*f)(int, int, ...));

v4si cb_add(v4si a, v4si b) { return a * b + a; }
v4si cb9(v4si a, v4si b, v4si c, v4si d, v4si e, v4si f, v4si g, v4si h, v4si i)
{
    return a + b + c + d + e + f + g + h + i + i;
}
long long cb_va(int n, ...)
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
double cb_d9(double a, double b, double c, double d, double e, double f, double g, double h, double i)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i;
}
int cb_i10(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j;
}
long double cb_ld7(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long double x, long a8)
{
    return x * 2 + (long double) (a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8);
}
long double cb_vld2(int n, int pad, ...)
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
static void pv(const char *tag, v4si v) { printf("%s %d %d %d %d\n", tag, v[0], v[1], v[2], v[3]); }
int main(void)
{
    v4si one = {1, 1, 1, 1}, a = {1, 2, 3, 4}, b = {10, 20, 30, 40};
    v2df d = {0.5, 1.5}, e = {0.25, 0.75}, f;
    pv("nadd", nadd(a, b));
    pv("nsum9", nsum9(one, one, one, one, one, one, one, one, b));
    pv("nmixed", nmixed(7, b, 11, one, 13));
    pv("nstackmix", nstackmix(1, 2, 3, 4, 5, 6, 7, one, one, one, one, one, one, one, one, b));
    f = ndsum(d, e);
    printf("ndsum %.2f %.2f\n", f[0], f[1]);
    printf("nvsum %lld\n", nvsum(3, 0, 7, 2, a, 1, 2.0));
    printf("nvsum %lld\n", nvsum(4, 2, a, 2, b, 3, d, 0, 100));
    printf("nvsum %lld\n", nvsum(10, 1, 1.0, 1, 1.0, 1, 1.0, 1, 1.0, 1, 1.0, 1, 1.0, 1, 1.0, 1, 1.0, 2, b, 3, d));
    pv("napply", napply(cb_add, a, b));
    pv("napply9", napply9(cb9, one, b));
    printf("napplyva %lld\n", napplyva(cb_va, a, d));
    printf("dapply9 %.1f\n", dapply9(cb_d9));
    printf("iapply10 %d\n", iapply10(cb_i10));
    printf("nld7 %.1Lf\n", nld7(1, 2, 3, 4, 5, 6, 7, 100.5L, 8));
    printf("nvld %.2Lf\n", nvld(3, 100, 0, 1L, 0, 2L, 1, 0.25L));
    printf("ldapply %.1Lf\n", ldapply(cb_ld7));
    printf("vldapply %.2Lf\n", vldapply(cb_vld2));
    return 0;
}
