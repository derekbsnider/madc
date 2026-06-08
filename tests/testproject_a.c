#include <stdio.h>
static int secret = 100;        /* file-local; same name as b.c */
int from_a(void) { return secret; }
extern int from_b(void);
int main(void) {
	printf("a_secret=%d from_b=%d\n", from_a(), from_b());
	return 0;
}
