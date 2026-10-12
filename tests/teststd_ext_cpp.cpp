// The standard by extension (split arc 1A.4): a .cpp source with no --std
// compiles as plain g++ compiles it — gnu++17: C++17 with GNU extensions.
// ORACLE: g++ 13 and clang++ 18 with no -std print these lines exactly.
// (__STRICT_ANSI__ is not asked: madc's C++ modes keep the strict
// presentation until __float128 / _FloatN land — the KNOWN DIVERGENCE at
// Program::strict_ansi_mode, include/madc.h.)
#include <cstdio>

namespace outer::inner { inline int seven = 7; }	// C++17: nested namespace, inline variable

int main()
{
	std::printf("cplusplus: %ld\n", (long)__cplusplus);
	int x = 0;
	if constexpr (sizeof(int) == 4)	// C++17: if constexpr
		x = outer::inner::seven;
	bool t = true;
	std::printf("cxx17: %d %d\n", x, (int)t);
	return 0;
}
