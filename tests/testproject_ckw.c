#include <stdio.h>
/* C++ keyword `class` used as an ordinary C identifier — legal C, and the
   build driver must compile a .c file in C mode (like gcc/clang) so this
   parses. Regression test for the if-condition elaborated-type-specifier bug. */
int main(void)
{
	int class = 5;
	if ( class >= 3 || class < 0 )
		printf("class ok: %d\n", class);
	return 0;
}
