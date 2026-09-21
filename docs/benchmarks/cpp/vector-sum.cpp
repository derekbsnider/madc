/* Container growth + iteration: push_back into std::vector, then sum. */
#include <vector>
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv) {
	int reps = (argc == 2) ? atoi(argv[1]) : 1;
	long total = 0;
	for (int r = 0; r < reps; ++r) {
		std::vector<int> v;
		for (int i = 0; i < 20000; ++i) v.push_back(i);
		for (size_t i = 0; i < v.size(); ++i) total += v[i];
	}
	printf("sum: %ld\n", total);
	return 0;
}
