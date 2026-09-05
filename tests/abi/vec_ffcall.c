/* The c2m half of the vector ABI interop gate (see libvecnative.c): calls the
   native functions with vectors (declared, nine-deep, mixed with integers,
   varargs) and hands them callbacks that receive vectors, nine vectors, a
   vector vararg list, nine doubles and ten ints -- and the stack-argument
   packing probes (ten ints / chars / shorts / floats, a mixed run, a vararg
   function with a named stack argument) in both directions.  Oracle: the same
   file compiled by the platform compiler and linked with libvecnative.c. */
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
struct S2 { long a, b; };
int nisum10(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10);
int ncsum10(char a1, char a2, char a3, char a4, char a5, char a6, char a7, char a8, char a9, char a10);
int nssum10(short a1, short a2, short a3, short a4, short a5, short a6, short a7, short a8, short a9, short a10);
float nfsum10(float a1, float a2, float a3, float a4, float a5, float a6, float a7, float a8, float a9, float a10);
long nmixpack(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8,
              char c, int i, char c2, long l, short s, struct S2 st, char c3, float f);
long nva9(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, int a9, ...);
int capply10(int (*f)(char, char, char, char, char, char, char, char, char, char));
int sapply10(int (*f)(short, short, short, short, short, short, short, short, short, short));
float fapply10(float (*f)(float, float, float, float, float, float, float, float, float, float));
long mixapply(long (*f)(long, long, long, long, long, long, long, long,
                        char, int, char, long, short, struct S2, char, float));
long va9apply(long (*f)(long, long, long, long, long, long, long, long, int, ...));

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
/* the packing probes' callbacks: the c2m side is the CALLEE of a native caller */
int cb_c10(char a1, char a2, char a3, char a4, char a5, char a6, char a7, char a8, char a9, char a10)
{
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + 9 * a9 + 10 * a10;
}
int cb_s10(short a1, short a2, short a3, short a4, short a5, short a6, short a7, short a8, short a9, short a10)
{
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + 9 * a9 + 10 * a10;
}
float cb_f10(float a1, float a2, float a3, float a4, float a5, float a6, float a7, float a8, float a9, float a10)
{
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + 9 * a9 + 10 * a10;
}
long cb_mix(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8,
            char c, int i, char c2, long l, short s, struct S2 st, char c3, float f)
{
    return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + c * 1000000L + i * 100000L + c2 * 10000L
           + l * 1000L + s * 100L + (st.a + st.b) * 10L + c3 * 3L + (long) f;
}
long cb_va9(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, int a9, ...)
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
    {
	struct S2 st = {100, 200};
	printf("nisum10 %d\n", nisum10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
	printf("ncsum10 %d\n", ncsum10(1, 2, 3, 4, 5, 6, 7, 8, -9, -10));
	printf("nssum10 %d\n", nssum10(1, 2, 3, 4, 5, 6, 7, 8, -900, -1000));
	printf("nfsum10 %.2f\n", nfsum10(1, 2, 3, 4, 5, 6, 7, 8, 9.5f, 10.25f));
	printf("nmixpack %ld\n", nmixpack(1, 2, 3, 4, 5, 6, 7, 8, -9, 10, 11, 12, -13, st, 14, 15.5f));
	printf("nva9 %ld\n", nva9(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11L, 12));
	printf("capply10 %d\n", capply10(cb_c10));
	printf("sapply10 %d\n", sapply10(cb_s10));
	printf("fapply10 %.2f\n", fapply10(cb_f10));
	printf("mixapply %ld\n", mixapply(cb_mix));
	printf("va9apply %ld\n", va9apply(cb_va9));
    }
    return 0;
}
