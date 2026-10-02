// TU a of tests/testprojectmtiorder: S(int), Box::count(int), and
// map::operator[] with an rvalue key (operator[](key_type&&)).
#include "testprojectmtiorder.h"

int part_a()
{
	S s(1);
	Box b;
	int n = b.count(1);
	std::map<std::string, int> m;
	m["alpha"] = 1;
	return s.v * 100 + n * 10 + (int)m.size();
}
