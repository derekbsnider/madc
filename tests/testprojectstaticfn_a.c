#include <stdio.h>
/* C internal linkage for FUNCTIONS: both TUs define static helper() and
 * static inline twice() with DIFFERENT bodies — each TU must call its own
 * copy (gcc/clang oracle: a=2 b=300). Pre-fix, madc emitted statics with
 * external linkage: the two-module MIR link died with "func helper is
 * prohibited for redefinition" (same root as the AOT-ledger rt_dump.h
 * static-inline pair). */
static int helper(void) { return 1; }
static inline int twice(int v) { return 2 * v; }
int from_a(void) { return twice(helper()); }
extern int from_b(void);
int main(void)
{
	printf("a=%d b=%d\n", from_a(), from_b());
	return 0;
}
