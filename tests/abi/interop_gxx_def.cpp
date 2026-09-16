// Reverse lane: g++ DEFINES the header's members; madc (interop_madc_use.mad)
// uses them — a madc TU whose declared-only user members must resolve to the
// symbols a real C++ TU exports, and whose report() the g++ side calls back.
#include <cstdio>
#include "interop_class.h"

static int live_counters = 0;

Counter::Counter() : n(0) { ++live_counters; }
Counter::Counter(int start) : n(start) { ++live_counters; }
Counter::~Counter() { --live_counters; }
int Counter::add(int d) { n += d; return n; }
int Counter::add(const char *digits)
{
    for (const char *p = digits; *p; ++p)
	n += *p - '0';
    return n;
}
int Counter::get() const { return n; }
int Counter::instances() { return live_counters; }
Counter &Counter::operator+=(int d) { n += d; return *this; }
bool Counter::operator==(const Counter &o) const { return n == o.n; }
int Counter::weight() { return n * 2; }

namespace tally {
    Ledger::Ledger() : total(0) {}
    void Ledger::post(int amount) { total += amount; }
    void Ledger::post(double amount) { total += (int)(amount * 100); }
    int Ledger::balance() const { return total; }
    int checksum(int a, int b) { return a * 31 + b; }
    int checksum(const char *s) { int h = 0; while (*s) h = h * 7 + *s++; return h; }
    namespace audit { int stamp() { return 2026; } }
    int scale_check(int v) { return scale(v, 3) + (int)scale(0.5, v * 4); }
}

int other_side_report(const Counter &c)
{
    return report(c) + 1;
}
