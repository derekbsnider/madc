// Shared header of tests/testprojectmtiorder (see that file): one constructor
// template, one variadic member template, and std::map's key-taking
// operator[] — each instantiated with DIFFERENT arguments in the two TUs.
#include <map>
#include <string>
#include <vector>

struct S {
	template<class T> S(T) : v((int)sizeof(T)) {}
	int v;
};

struct Box {
	template<class... A> int count(A...) { return (int)sizeof...(A); }
};
