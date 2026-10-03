// TU b of tests/testprojectmtiorder: S(double), Box::count(int, double,
// char), and map::operator[] with an lvalue key (operator[](const
// key_type&) — the key is copied, never moved).
#include "testprojectmtiorder.h"

int part_b()
{
	S s(2.0);
	Box b;
	int n = b.count(1, 2.0, 'c');
	std::vector<std::string> w;
	w.push_back("one");
	std::map<std::string, int> m;
	m[w[0]] = 5;
	return s.v * 1000 + n * 100 + (int)w[0].size() * 10 + m["one"];
}
