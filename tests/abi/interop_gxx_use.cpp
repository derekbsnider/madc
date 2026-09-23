// g++ side of the interop lane: USES the class madc defined — constructs on
// the stack and the heap, calls the type-overloaded members, the const and
// static ones, the operators, overrides a virtual (a derived class whose
// ctor calls madc's _ZN7CounterC2Ei / dtor calls _ZN7CounterD2Ev, whose
// vtable chain references _ZTV7Counter / _ZTI7Counter), deletes through a
// base pointer (madc's _ZN7CounterD0Ev), and defines report() for madc's
// caller. Expected output is what g++ produces when BOTH TUs are g++.
#include <cstdio>
#include "interop_class.h"

int other_side_report(const Counter &c);

static int fnref_twice(int x) { return x * 2; }
int report(const Counter &c)
{
    return c.get() * 10;
}

struct Heavy : Counter {
    Heavy(int start) : Counter(start) {}
    ~Heavy() {}
    int weight() { return Counter::weight() + 100; }
};

int main()
{
    Counter a;
    Counter b(5);
    printf("add: %d %d\n", a.add(3), b.add("123"));
    printf("get: %d %d\n", a.get(), b.get());
    a += 4;
    printf("plus: %d eq=%d\n", a.get(), (int)(a == b));
    printf("instances: %d\n", Counter::instances());
    Counter *h = new Heavy(7);
    printf("weight: %d %d\n", b.weight(), h->weight());
    printf("report: %d\n", other_side_report(b));
    delete h;
    printf("instances after delete: %d\n", Counter::instances());
    tally::Ledger l;
    l.post(5);
    l.post(2.5);
    printf("balance: %d\n", l.balance());
    printf("checksum: %d %d %d\n", tally::checksum(3, 4), tally::checksum("abc"),
	   tally::audit::stamp());
    printf("scale_check: %d\n", tally::scale_check(4));
    Counter u(2);
    *u = 9;
    printf("deref: %d mul: %d\n", *u, u * 3);
    ++u;
    printf("preinc: %d\n", u.get());
    int old = u++;
    printf("postinc: %d %d\n", old, u.get());
    int arr[4] = { 1, 2, 3, 4 };
    printf("add4: %d\n", u.add4(arr));
    printf("cell: %d\n", cell_check(21));
    int (*tp)(int) = fnref_twice;
    printf("fnref: %d %d\n", via_fnptr_ref(tp, 5), via_fn_ref(fnref_twice, 6));
    return 0;
}
