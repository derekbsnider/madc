/* aarch64-linux long double: every MIR builtin, with EVERY operand built at
   run time from integers. Header-free so the AOT leg (madc-aarch64-linux -c,
   linked by aarch64 gcc) and the JIT leg (a gcc-built aarch64 c2m, -eg) read
   the identical source. Driven by scripts/aarch64_ldouble_lane.sh, whose oracle
   is this file compiled by aarch64-linux-gnu-gcc and run under the same qemu.

   aarch64-linux long double is IEEE binary128 with no instructions, so each
   operation is a libgcc soft-float call: add/sub/mul/div/neg (__addtf3 ...
   __negtf2), the seven conversions, and the six comparisons, whose libgcc
   routines return an ORDERING int -- the cmp-eq and cmp-nan lines pin that the
   0/1 comes out right for equal and unordered operands. 2^53+1 and UINT64_MAX
   are exact in binary128 and not in binary64, so a leg that silently computed
   in double cannot match. */
int printf (const char *, ...);
typedef long long int64_t;
typedef unsigned long long uint64_t;

int main (void)
{
	volatile int one = 1, two = 2, three = 3, zero = 0;
	long double a = (long double) one / (long double) three;
	long double b = (long double) two;
	volatile int64_t i = 9007199254740993LL;
	volatile uint64_t u = 18446744073709551615ULL;
	volatile float f = 0.1f;
	volatile double d = 0.1;
	long double s = a + b, df = a - b, m = a * b, q = a / b, n = -a;
	printf ("add %.30Lf\nsub %.30Lf\nmul %.30Lf\ndiv %.30Lf\nneg %.30Lf\n", s, df, m, q, n);
	printf ("i2ld %.1Lf\nui2ld %.1Lf\n", (long double) i, (long double) u);
	printf ("f2ld %.30Lf\nd2ld %.30Lf\n", (long double) f, (long double) d);
	printf ("ld2i %lld\n", (long long) ((long double) i + (long double) one / (long double) (4 * one)));
	printf ("ld2f %.9g\nld2d %.17g\n", (double) (float) a, (double) a);
	printf ("cmp %d %d %d %d %d %d\n", a == b, a != b, a < b, a >= b, a > b, a <= b);
	printf ("cmp-eq %d %d %d %d %d %d\n", a == a, a != a, a < a, a >= a, a > a, a <= a);
	long double z = (long double) zero;
	long double nan = z / z;
	printf ("cmp-nan %d %d %d %d %d %d\n", nan == a, nan != a, nan < a, nan >= a, nan > a, nan <= a);
	long double nz = -z;
	printf ("neg-zero-sign %d\n", (1.0L / nz) < z);   /* 1/-0 is -inf: __negtf2 flipped +0 */
	return 0;
}
