// mangle_corpus.cpp — the USER-SHAPE Itanium mangling oracle corpus.
//
// Rule #1: gcc/clang is canon. In --std=c++##/--std=madc mode every
// user-defined C++ symbol madc emits must be byte-identical to what g++ and
// clang++ emit for the same declaration, so a madc-compiled .o/.so links
// against real C++ (docs/plans/2026-09-16-free-function-overloading-linkage.md).
//
// This file is compiled by BOTH canon compilers (scripts/gen_mangle_oracle.sh):
// their defined-symbol sets must agree, and the agreed set is written to
// tests/unit/mangle_oracle.inc — the truth tests/unit/test_mangle.cpp replays
// against madc's Itanium encoders (phase 0) and, once madc emits these symbols
// itself, scripts/mangle_abi_gate.sh compares `madc -c` → nm against it
// (phase 5). Every function here is DEFINED (out of class, never inline) so a
// symbol is emitted for each; nothing has dynamic initialization, so the set
// is exactly the declared shapes. Growing the corpus = add a shape here,
// regenerate the .inc on the container, scp it back, add its encoder check.
//
// Shapes, in order: builtins · pointers/refs/cv · varargs/fn-ptr/array ·
// user class params (+ substitutions) · namespaced / nested · enums ·
// typedef desugar · overload set · the darwin `send` shape · namespace fns ·
// members / static / const · ctors / dtors · member operators (incl. unary,
// compound, conversion, ->, new/delete) · free operators · class template ·
// function template · virtual class (vtable / typeinfo) · extern "C" · main ·
// internal linkage.

// Deliberately stdlib-AGNOSTIC (only <cstddef> for operator new's size_t):
// a std:: parameter mangles differently per flavor (libstdc++ abbreviates
// std::ostream to `So`; libc++ spells std::__1::basic_ostream<…>), so one
// checked-in oracle could not be drift-gated on both the Linux and the Mac
// lane. std:: binding has its own flavor-aware tests in test_mangle.cpp.
#include <cstddef>

// ---- builtins -----------------------------------------------------------
void f_void() {}
void f_int(int) {}
void f_two(int, double) {}
void f_bool(bool) {}
void f_char(char) {}
void f_schar(signed char) {}
void f_uchar(unsigned char) {}
void f_short(short) {}
void f_ushort(unsigned short) {}
void f_uint(unsigned int) {}
void f_long(long) {}
void f_ulong(unsigned long) {}
void f_llong(long long) {}
void f_ullong(unsigned long long) {}
void f_float(float) {}
void f_ldouble(long double) {}
void f_wchar(wchar_t) {}
void f_char16(char16_t) {}
void f_char32(char32_t) {}
void f_size(std::size_t) {}

// ---- pointers / references / cv -----------------------------------------
void f_cstr(const char *) {}
void f_str(char *) {}
void f_ptr(int *) {}
void f_pptr(int **) {}
void f_vptr(void *) {}
void f_cvptr(const void *) {}
void f_ref(int &) {}
void f_cref(const int &) {}
void f_rref(int &&) {}
void f_kint(const int) {}          // top-level const is NOT part of the type
void f_cpref(const char *&) {}

// ---- varargs / function pointers / arrays -------------------------------
void f_va(const char *, ...) {}
void f_fp(int (*)(int)) {}
void f_fpv(void (*)()) {}
void f_arr(int[10]) {}             // decays to int*
void f_arr2(int[2][3]) {}          // decays to int (*)[3]      → PA3_i
void f_arr3(int[2][3][4]) {}       // decays to int (*)[3][4]   → PA3_A4_i
void f_arrp(int *[3]) {}           // array of pointers → int ** → PPi

// ---- user class params (+ substitution back-refs) -----------------------
struct Foo;
namespace ns { struct Bar; }
struct Foo {
	int x;
	// members (defined below)
	void m_void();
	void m_int(int);
	void m_cstr(const char *);
	int m_const(int) const;
	void m_self(Foo &);
	void m_two(const Foo &, const Foo &);
	void m_bar(ns::Bar &);
	static void m_static(int);
	// ctors / dtor
	Foo();
	Foo(int);
	Foo(const Foo &);
	~Foo();
	// operators
	Foo &operator=(const Foo &);
	bool operator==(const Foo &) const;
	bool operator!=(const Foo &) const;
	bool operator<(const Foo &) const;
	Foo operator+(const Foo &) const;   // binary +
	Foo operator+() const;              // unary +
	Foo operator-(const Foo &) const;   // binary -
	Foo operator-() const;              // unary -
	int &operator*();                   // unary * (deref)
	Foo operator*(int) const;           // binary *
	Foo *operator&();                   // unary & (address-of)
	Foo operator&(const Foo &) const;   // binary &
	Foo &operator+=(const Foo &);
	Foo &operator%=(int);
	Foo &operator<<=(int);
	bool operator&&(const Foo &) const;
	bool operator!() const;
	int &operator[](int);
	void operator()(int);
	Foo &operator++();                  // pre-increment
	Foo operator++(int);                // post-increment
	Foo *operator->();
	Foo &operator<<(int);
	operator bool() const;              // conversion
	operator int() const;
	static void *operator new(std::size_t);
	static void operator delete(void *);
};
void g_val(Foo) {}
void g_ref(Foo &) {}
void g_cref(const Foo &) {}
void g_ptr(Foo *) {}
void g_cptr(const Foo *) {}
void g_two(Foo &, Foo &) {}
void g_mix(const Foo &, Foo *) {}
Foo make_foo() { return Foo(); }

// ---- namespaced / nested classes ----------------------------------------
namespace ns {
	struct Bar {
		int y;
		void bm(int);
		void bm_foo(Foo &);
		Bar();
		~Bar();
	};
	enum Mode { M0, M1 };
	void nf_int(int) {}
	void nf_bar(Bar &) {}
	void nf_foo(Foo &) {}
}
namespace a { namespace b {
	struct C { int z; };
	void nf_deep() {}
} }
struct Outer {
	enum Kind { K0 };
	struct Inner {
		void im();
	};
	void om(Kind);
};
void h_ref(ns::Bar &) {}
void h_two(ns::Bar &, ns::Bar &) {}
void h_val(ns::Bar) {}
void h_deep(a::b::C &) {}
void i_nest(Outer::Inner &) {}
void i_kind(Outer::Kind) {}

// ---- enums ---------------------------------------------------------------
enum Color { RED, GREEN };
enum class Scoped { A, B };
void e_enum(Color) {}
void e_scoped(Scoped) {}
void e_ns(ns::Mode) {}

// ---- typedef desugar (Itanium encodes canonical types, never aliases) ----
typedef unsigned long ulong_t;
typedef Foo FooAlias;
void td_ulong(ulong_t) {}
void td_cls(FooAlias &) {}

// ---- the overload set (the motivating capability) ------------------------
void ov(int) {}
void ov(const char *) {}
void ov(double) {}
void ov(Foo &) {}

// ---- the darwin blocker #2 shape, verbatim --------------------------------
namespace madc { struct channel { int fd; }; struct value { int k; }; }
void send(madc::channel &, madc::value &) {}

// ---- member definitions (out of class → emitted) ------------------------
void Foo::m_void() {}
void Foo::m_int(int) {}
void Foo::m_cstr(const char *) {}
int Foo::m_const(int v) const { return v; }
void Foo::m_self(Foo &) {}
void Foo::m_two(const Foo &, const Foo &) {}
void Foo::m_bar(ns::Bar &) {}
void Foo::m_static(int) {}
Foo::Foo() : x(0) {}
Foo::Foo(int v) : x(v) {}
Foo::Foo(const Foo &o) : x(o.x) {}
Foo::~Foo() {}
Foo &Foo::operator=(const Foo &) { return *this; }
bool Foo::operator==(const Foo &) const { return true; }
bool Foo::operator!=(const Foo &) const { return false; }
bool Foo::operator<(const Foo &) const { return false; }
Foo Foo::operator+(const Foo &) const { return *this; }
Foo Foo::operator+() const { return *this; }
Foo Foo::operator-(const Foo &) const { return *this; }
Foo Foo::operator-() const { return *this; }
int &Foo::operator*() { return x; }
Foo Foo::operator*(int) const { return *this; }
Foo *Foo::operator&() { return this; }
Foo Foo::operator&(const Foo &) const { return *this; }
Foo &Foo::operator+=(const Foo &) { return *this; }
Foo &Foo::operator%=(int) { return *this; }
Foo &Foo::operator<<=(int) { return *this; }
bool Foo::operator&&(const Foo &) const { return true; }
bool Foo::operator!() const { return false; }
int &Foo::operator[](int) { return x; }
void Foo::operator()(int) {}
Foo &Foo::operator++() { return *this; }
Foo Foo::operator++(int) { return *this; }
Foo *Foo::operator->() { return this; }
Foo &Foo::operator<<(int) { return *this; }
Foo::operator bool() const { return true; }
Foo::operator int() const { return x; }
void *Foo::operator new(std::size_t n) { return ::operator new(n); }
void Foo::operator delete(void *p) { ::operator delete(p); }

void ns::Bar::bm(int) {}
void ns::Bar::bm_foo(Foo &) {}
ns::Bar::Bar() : y(0) {}
ns::Bar::~Bar() {}
void Outer::Inner::im() {}
void Outer::om(Kind) {}

// ---- free operators ------------------------------------------------------
bool operator==(const ns::Bar &, const ns::Bar &) { return true; }
Foo operator+(const Foo &, int) { return Foo(); }
Foo &operator<<(Foo &f, int) { return f; }

// ---- class template (explicit instantiation → every member emitted) ------
template <class T> struct Box {
	T v;
	Box();
	void put(T);
	T get() const;
	template <class U> U conv() const;   // member template, NO parameters → v
};
template <class T> Box<T>::Box() : v() {}
template <class T> void Box<T>::put(T t) { v = t; }
template <class T> T Box<T>::get() const { return v; }
template <class T> template <class U> U Box<T>::conv() const { return U(); }
template struct Box<int>;
template struct Box<Foo>;
template long Box<int>::conv<long>() const;
void t_box(Box<int> &) {}
void t_boxfoo(Box<Foo> &) {}
void t_nsbox(Box<ns::Bar> &) {}

// ---- function template ---------------------------------------------------
template <class T> T ident(T v) { return v; }
template int ident<int>(int);
template Foo ident<Foo>(Foo);
template <class T> T make() { return T(); }   // NO parameters → v
template int make<int>();
namespace ns {
	template <class T> T nident(T v) { return v; }   // namespaced
	template int nident<int>(int);
}
// A dependent nested-name parameter: the `typename` disambiguator never reaches
// the symbol (RN2rrIT_E4typeE); a reference template argument is IRiE, a
// pointer one IPiE (two products must never fold onto one symbol); a
// non-template parameter beside T stays `i` (T_ vs i).
template <class T> struct rr { typedef T type; };
template <class T> struct rr<T &> { typedef T type; };
template <class T> T &&fwd(typename rr<T>::type &v) { return static_cast<T &&>(v); }
template <class T> T &&fwd(typename rr<T>::type &&v) { return static_cast<T &&>(v); }
template int &fwd<int &>(rr<int &>::type &);
template int &&fwd<int>(rr<int>::type &&);
template <class T> T scale(T v, int k) { return v * k; }
template int scale<int>(int, int);
namespace ns {
	template <class T> T &&nfwd(typename rr<T>::type &v) { return static_cast<T &&>(v); }
	template int *&&nfwd<int *>(rr<int *>::type &);
}

// ---- virtual class → vtable / typeinfo / D0-D1-D2 ------------------------
struct V {
	virtual void vf();
	virtual ~V();
};
void V::vf() {}
V::~V() {}
namespace ns {
	struct VB {
		virtual void vbf();
		virtual ~VB();
	};
	void VB::vbf() {}
	VB::~VB() {}
}

// ---- extern "C" opts out; main is bare; static still mangles -------------
extern "C" void c_fn(int) {}
extern "C" { void c_fn2() {} }
static void s_fn(int) {}
void s_user() { s_fn(1); }
int main() { return 0; }
