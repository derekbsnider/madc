// madc <-> g++ link-interop corpus (C++ symbol mangling, phase 2). ONE header
// both compilers read: madc defines these members in interop_madc_def.mad, a
// g++ TU (interop_gxx_use.cpp) constructs, calls, overrides and deletes them —
// every name crossing the object boundary is an Itanium symbol both sides
// must spell identically (_ZN7Counter3addEi, _ZN7CounterC1Ei, _ZTV7Counter,
// _ZN7CounterD0Ev, ...). The reverse direction (interop_gxx_def.cpp defines,
// interop_madc_use.mad uses) shares the header too.
#ifndef INTEROP_CLASS_H
#define INTEROP_CLASS_H 1

struct Counter {
    int n;
    Counter();
    Counter(int start);
    virtual ~Counter();
    int add(int d);
    int add(const char *digits);
    int get() const;
    static int instances();
    Counter &operator+=(int d);
    bool operator==(const Counter &o) const;
    virtual int weight();
};

namespace tally {
    struct Ledger {
	int total;
	Ledger();
	void post(int amount);
	void post(double amount);
	int balance() const;
    };
    // Namespace FUNCTIONS (phase 3): overloads and a nested namespace, defined
    // by the definer side under _ZN5tally8checksumEii / …EPKc / _ZN5tally5audit5stampEv.
    int checksum(int a, int b);
    int checksum(const char *s);
    namespace audit { int stamp(); }
    // A header function template (phase 3b): both definers instantiate
    // scale<int> / scale<double>, so each object defines the same weak
    // _ZN5tally5scaleIiEET_S1_i — an internal product name would be alien.
    template <class T> T scale(T v, int k) { return v * k; }
    int scale_check(int v);
}

// Defined by the OTHER side of each lane (the user TU): the definer calls it
// so a symbol also flows definer <- user.
int report(const Counter &c);

#endif
