/* Call-heavy floating point: the sentinel for the v0.93.0 MIR
 * false-dependency fix (pxor before the merging scalar SSE converts).
 *
 * The defect: MIR emitted cvtsi2sd/cvtsd2ss etc. bare, so every convert
 * falsely depended on the destination register's previous writer. In a loop
 * that interleaves int->double conversion with libm calls, that previous
 * writer is the last call's return value, which serialises calls the
 * out-of-order core would otherwise overlap (~76 vs ~24 cycles/call at
 * IDENTICAL instruction counts — invisible to callgrind, misattributed to
 * libm by samplers).
 *
 * So the shape that matters is: int -> double convert, feed it to sin/cos,
 * accumulate, repeat. Deterministic output, bounded iterations, no sleep.
 * (donut.c is unusable as a benchmark: for(;;) with usleep(30000).)
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main (int argc, char *argv[]) {
	long reps = (argc == 2) ? atol (argv[1]) : 1;
	double acc = 0.0;
	long r, i;

	for (r = 0; r < reps; r++) {
		for (i = 0; i < 1000; i++) {
			/* int -> double convert feeding a libm call: the chain the
			 * bare convert encoding serialised. */
			double a = (double) i;
			double b = (double) (i & 63);
			float  f = (float) (i & 15);		/* cvtsi2ss + cvtss2sd */
			acc += sin (a) * cos (b) + (double) f;
			acc += sqrt ((double) (i + 1));
		}
	}
	/* Print with a fixed precision so the comparison is exact across
	 * compilers without depending on the last ulp of a long accumulation. */
	printf ("acc: %.6f\n", acc / (double) reps);
	return 0;
}
