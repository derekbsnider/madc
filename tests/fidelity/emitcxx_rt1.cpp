// emitcxx round-trip reducer 1 (AST-4): std::string with escapes, a class
// with ctor-init/const method, a function-like macro, a global with an
// initializer, comments. The gate compiles madc's --emit=c++ render with
// g++ AND clang++ and compares runs against the original.
#include <string>
#include <cstdio>

#define TWICE(x) ((x) + (x))

int counter = 7;                 // global with initializer

class Box {
    int v;
public:
    Box(int n) : v(n) {}
    int get() const { return v; }
};

static int helper(int a)
{
    /* block comment */
    return TWICE(a) - 1;
}

int main()
{
    std::string s = "he\tsaid \"hi\"\n";
    s += "tail";
    Box b(counter);
    printf("len=%zu box=%d h=%d\n", s.size(), b.get(), helper(4));
    return 0;
}
