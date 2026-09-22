/* Virtual dispatch through a base pointer — the vtable path. */
#include <cstdio>
#include <cstdlib>
struct Shape { virtual ~Shape() {} virtual long area(long x) const = 0; };
struct Sq : Shape { long area(long x) const { return x * x; } };
struct Tr : Shape { long area(long x) const { return x * x / 2; } };
int main(int argc, char **argv) {
	int reps = (argc == 2) ? atoi(argv[1]) : 1;
	Sq s; Tr t;
	Shape *shapes[2]; shapes[0] = &s; shapes[1] = &t;
	long total = 0;
	for (int r = 0; r < reps; ++r)
		for (long i = 0; i < 20000; ++i)
			total += shapes[i & 1]->area(i & 255);
	printf("total: %ld\n", total);
	return 0;
}
