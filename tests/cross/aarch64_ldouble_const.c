/* aarch64-linux long double CONSTANTS, in every route a constant's bytes take
   to the target: a folded expression and a literal (simplify's
   immediate-to-memory step), a static scalar initializer, a static array, a
   static struct member and a _Complex long double (c2mir's data initializers).

   The x86-hosted cross compiler used to write each of these in the HOST's x87
   byte layout, which aarch64 reads as a binary128 near zero. Every value here
   is EXACTLY representable in x87's 64-bit significand, so folding it on the
   host loses nothing and the output is byte-identical to gcc's. (A constant
   that is NOT -- 1.0L/3.0L -- is folded at host precision by the cross
   compiler: 64 significand bits, not 113. That is a separate, documented
   cross-fidelity limit, and deliberately not asserted here.) */
int printf (const char *, ...);

static long double s_scalar = 1.5L;
static long double s_array[4] = { 2.0L, -0.5L, 1024.25L, 3.0L };
static struct { int tag; long double v; } s_member = { 7, -6.125L };
static _Complex long double s_cplx = 2.5L + 0.75iL;

int main (void)
{
	volatile int one = 1;
	long double folded = 3.0L * 0.25L + 1.0L;          /* folded on the host: 1.75 */
	long double lit = 1099511627776.0L;                /* 2^40 */
	long double big = 18446744073709551615.0L;         /* UINT64_MAX: 64 bits, exact in x87 */
	long double tiny = 0x1p-16000L;                     /* far below binary64's range */
	printf ("folded %.30Lf\n", folded * (long double) one);
	printf ("lit %.1Lf\nbig %.1Lf\n", lit * (long double) one, big * (long double) one);
	printf ("tiny %.6Le\n", tiny * (long double) one);
	printf ("scalar %.30Lf\n", s_scalar * (long double) one);
	for (int k = 0; k < 4; k++)
		printf ("array%d %.30Lf\n", k, s_array[k] * (long double) one);
	printf ("member %d %.30Lf\n", s_member.tag, s_member.v * (long double) one);
	printf ("cplx %.30Lf %.30Lf\n", __real__ s_cplx * (long double) one, __imag__ s_cplx * (long double) one);
	printf ("neg-zero-lit %d\n", (1.0L / (-0.0L * (long double) one)) < 0.0L);
	return 0;
}
