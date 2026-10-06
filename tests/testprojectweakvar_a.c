#include <stdio.h>
/* A weak OBJECT definition yields to a strong one ([ELF] STB_WEAK; gcc
 * __attribute__((weak))), as a weak function does (testprojectweakfn): wa is
 * weak here and strong in b, wb the other way round; the head attribute of a
 * declarator list is every declarator's (wf, wg), a declarator's own is its
 * object's only (wc, not wd); an array (we) binds like a scalar. madc bound
 * every weak object strong — the attribute reached functions only — so its
 * object file listed `D` where gcc's lists `V`, and the definitions collided.
 * gcc/clang oracle: wa=2 wb=3 bb=3 wc=6 wd=7 we=10,11 wf=14 wg=15. */
__attribute__((weak)) int wa = 1;
int wb = 3;
int wc __attribute__((weak)) = 5, wd = 7;
__attribute__((weak)) int we[2] = {8, 9};
__attribute__((weak)) int wf = 12, wg = 13;
int read_wb_from_b(void);

int main(void)
{
	printf("wa=%d wb=%d bb=%d wc=%d wd=%d we=%d,%d wf=%d wg=%d\n",
	       wa, wb, read_wb_from_b(), wc, wd, we[0], we[1], wf, wg);
	return 0;
}
