#include <stdio.h>
/* A weak definition yields to a strong one ([ELF] STB_WEAK; gcc
 * __attribute__((weak))): f is weak here and strong in b, g the other way
 * round, so both TU orders are covered. madc dropped the attribute (it was
 * not in the GNU attribute table), so every weak definition was strong:
 * --project kept both copies and each TU called its own, f() == 1 here and
 * two addresses for one function (B24). With the attribute kept, MIR_link
 * still gave the replaced weak body an interface, last (it links modules in
 * reverse load order), so the shared thunk ran it: f() == 1, one address.
 * gcc/clang oracle: f=2 g=3 gb=3 h=6 p=8 same=1. */
__attribute__((weak)) int f(void) { return 1; }
int g(void) { return 3; }
int __attribute__((weak)) h(void) { return 5; }
int p(void) __attribute__((weak));
int p(void) { return 7; }
int (*get_f(void))(void);
int g_from_b(void);

int main(void)
{
	printf("f=%d g=%d gb=%d h=%d p=%d same=%d\n",
	       f(), g(), g_from_b(), h(), p(), f == get_f());
	return 0;
}
