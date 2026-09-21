/* std::string append + compare — the libstdc++ interop path. */
#include <string>
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv) {
	int reps = (argc == 2) ? atoi(argv[1]) : 1;
	long n = 0;
	for (int r = 0; r < reps; ++r) {
		std::string s;
		for (int i = 0; i < 2000; ++i) s += "abcdefgh";
		if (s.size() > 100) ++n;
		n += (long)s.size() / 1000;
	}
	printf("n: %ld\n", n);
	return 0;
}
