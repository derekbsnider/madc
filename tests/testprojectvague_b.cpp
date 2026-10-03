#include <stdio.h>
#include <typeinfo>
#include "testprojectvague.h"
B *make_c();
int count_in_a();
int main() {
	B *bp = make_c();
	int same = typeid(*bp) == typeid(C);
	int okc = dynamic_cast<C *>(bp) != 0;
	int c1 = count_in_a();
	int c2 = counter();
	printf("same=%d okc=%d c1=%d c2=%d\n", same, okc, c1, c2);
	return 0;
}
