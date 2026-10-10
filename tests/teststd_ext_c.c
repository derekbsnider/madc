/* The standard by extension (split arc 1A.4): a .c source with no --std
   compiles as plain gcc compiles it — gnu17: C17 with GNU extensions, no
   __STRICT_ANSI__, the C++ keywords ordinary identifiers. ORACLE: gcc 13 and
   clang 18 with no -std print these lines exactly. */
#include <stdio.h>

int main(void)
{
	int class = 1, new = 2;		/* C++ keywords are C identifiers */
	printf("stdc_version: %ld\n", (long)__STDC_VERSION__);
#ifdef __STRICT_ANSI__
	printf("strict: yes\n");
#else
	printf("strict: no\n");
#endif
#ifdef __cplusplus
	printf("cplusplus: yes\n");
#else
	printf("cplusplus: no\n");
#endif
	printf("identifiers: %d\n", class + new);
	return 0;
}
