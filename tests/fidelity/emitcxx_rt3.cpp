// emitcxx round-trip reducer 3 (the compound-type-specifier lookahead):
// `char *` / `long *` heads whose rejected lookahead consumed the space
// (echoed `char*s` before the fix), multi-word specifiers, long double.
// The gate compiles madc's --emit=c++ render with g++ AND clang++ and
// compares runs against the original.
#include <cstdio>

static const char *label = "rt3";

static unsigned long long twice_u(unsigned long long v)
{
    return v * 2ULL;
}

static long double scale(long double d)
{
    return d * 4.0L;
}

int main()
{
    char buf[8];
    char *p = buf;
    *p = 'x';
    p[1] = '\0';
    long total = (long)twice_u(21ULL);
    printf("%s %s total=%ld sc=%.1Lf\n", label, buf, total, scale(2.5L));
    return 0;
}
