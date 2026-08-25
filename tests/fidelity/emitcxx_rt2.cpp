// emitcxx round-trip reducer 2 (AST-4): a function template, overloads,
// an enum, switch-in-loop control flow, references.
#include <string>
#include <cstdio>

enum Color { RED = 1, GREEN = 2, BLUE = 4 };

template <typename T>
T maxi(T a, T b) { return a < b ? b : a; }

static int describe(const std::string &s) { return (int)s.size(); }
static int describe(int n) { return n * 10; }

static void bump(int &r) { r += 3; }

int main()
{
    int acc = 0;
    for ( int i = 0; i < 5; ++i )
    {
	switch ( i & 3 )
	{
	case 0: acc += RED; break;
	case 1: acc += GREEN; break;
	default: acc += BLUE; break;
	}
    }
    bump(acc);
    std::string t = "abc";
    t += "de";
    printf("acc=%d m=%ld d1=%d d2=%d\n", acc, maxi(3L, 9L), describe(t),
	   describe(7));
    return 0;
}
