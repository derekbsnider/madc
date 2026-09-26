// Shared by testprojectvague_a.cpp and testprojectvague_b.cpp: every TU that
// includes it emits its own linkonce copy of the vtables, the type_infos and
// counter() (with its static local).
struct B { virtual int f() { return 1; } };
struct C : B { int f() { return 2; } };
inline int counter() { static int n = 0; return ++n; }
