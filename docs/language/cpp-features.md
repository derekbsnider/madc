# Supported C++ Language Features

What C++ works in madc **today**. This is the user-facing catalog; the
compliance roadmap (what's planned, in what order) lives in
[`docs/plans/cpp-support.md`](../plans/cpp-support.md), and the measured
proof is the test suite — the whole of it passes under both stdlib flavors
(libstdc++ and, via `-stdlib=libc++`, libc++), which is only possible
because the features below survive contact with real standard-library
headers.

Everything here works in the default madc dialect and under the explicit
`--std=c++NN` modes (feature-gated by standard where the standard demands
it). C++ lowers to C11 in the `cir_node` tree — the Cfront model — so every
feature also flows through `--emit=c11` and native output.

## Classes and objects

- Classes and structs with methods, constructors (default, copy,
  converting, delegating mem-init chains), destructors, RAII scope
  destruction on every exit path
- Access control (`public` / `protected` / `private` — members default
  private in classes, public in structs) enforced against the *selected*
  overload
- **Inheritance: single, multiple, and virtual** — full Itanium layout:
  vtable groups (including transitive secondary vtables), virtual-base
  offsets, thunks, hidden vbase constructor parameters (madc's
  construction-vtables equivalent)
- Virtual dispatch, pure virtual functions, RTTI (`typeid`,
  `dynamic_cast`)
- Operator overloading (member and free, including free operator
  templates), conversion operators (including cv-qualified and
  reference-returning conversion-type-ids), `operator<=>` with rewritten
  candidates
- Static members (with brace-or-equal initializers), nested classes,
  NSDMIs (`int x{42};` and `= init` forms), anonymous aggregates and
  bit-fields
- Aggregate list-initialization (`S{a, b}`, `= {a, b}`, designators in C
  mode), `new` / `delete` / `new[]` / `delete[]` with Itanium array
  cookies, placement forms, explicit destructor calls

```c
#include <cstdio>

struct Shape {
	virtual ~Shape() {}
	virtual int area() const = 0;
};

struct Rect : Shape {
	int w, h;
	Rect(int w_, int h_) : w(w_), h(h_) {}
	int area() const { return w * h; }
};

int main()
{
	Shape *s = new Rect(6, 7);
	printf("%d\n", s->area());
	delete s;
	return 0;
}
```

## Templates

- Class templates, function templates, member templates (including member
  constructor templates), variadic packs (`sizeof...` is a real
  operator), fold-style pack expansion in bases, mem-inits, call
  arguments and `noexcept` clauses
- Partial and full specialization, template-template parameters
  (**including TTP defaults that name a different template** —
  [temp.param]p11, the libc++ `tuple()` idiom), non-type template
  parameters with constant-expression arguments
- SFINAE (`enable_if` idioms, unevaluated-operand viability), the
  `noexcept(expr)` operator, `decltype`, alias templates, variable
  templates, deduction guides ([temp.deduct.guide])
- **C++20 abbreviated function templates** (member form): `auto`
  parameters desugar to an invented template head ([dcl.fct]/18)
- C++20 **conditional `explicit(cond)`** constructors
- Instantiation is **parse-once**: patterns parse one time and
  instantiate by re-running the generic resolver over the saved tree (the
  g++ tsubst model) — never by re-lexing source text

```c
#include <cstdio>

template <class T, class... Rest>
struct first_of { typedef T type; };

template <class T> int sum(T v) { return v; }
template <class T, class... Rest>
int sum(T v, Rest... rest) { return v + sum(rest...); }

int main()
{
	first_of<int, double, char>::type x = 41;
	printf("%d %d\n", x + 1, sum(1, 2, 3, 4));
	return 0;
}
```

## Exceptions and error handling

- `try` / `catch` / `throw` with type-dispatched catch clauses, lowered to
  `setjmp`/`longjmp` frames; RAII cleanup runs on unwind
- `noexcept` specifications (including conditional `noexcept(expr)`),
  evaluated as a real operator in trait contexts

## Lambdas and functions

- Lambdas (madc spelling: typed return inside the brackets —
  `[int](int a, int b) { return a + b; }`), hoisted to free functions
- Function pointers through `auto`, overload resolution across free,
  member, static, and template candidates ([over.match] ranking,
  implicit-object-parameter const rules)
- Default arguments, reference parameters and returns (lowered to
  pointers), multiple return values (madc dialect)

## The standard library is real

madc does not ship stand-in implementations of the C++ library. It parses
the **real installed headers** and resolves symbols **mangled-direct**
(Itanium) against the real libstdc++ or libc++:

- `std::string` is the real class — construction, assignment, operators,
  `c_str()`/`size()`, concatenation, comparison against both flavors' ABIs
- Streams are real: `cout`/`cin`/`cerr`, `stringstream`, file streams,
  manipulators, `operator<<`/`>>` chains, formatted extraction
- Containers monomorphize from the real headers: `vector`, `map`, `set`,
  `tuple`, `optional`, `unique_ptr`, iterators and range-based `for`
- Type traits evaluate correctly against both stdlib implementations
  (`is_constructible`, `is_trivially_*`, conjunction/detection idioms)

```c
#include <iostream>
#include <vector>
#include <string>
using namespace std;

int main()
{
	vector<int> v;
	v.push_back(40);
	v.push_back(2);
	int sum = 0;
	for (int n : v)
		sum += n;
	string label = "sum=";
	cout << label << sum << endl;
	return 0;
}
```

## Selected C++11–C++23 checklist

| Feature | Status |
|---------|--------|
| `auto`, range-`for`, `nullptr`, scoped enums (incl. fixed bases driving layout) | ✅ |
| `constexpr` values (folded at compile time), `static_assert` | ✅ |
| References (lvalue/rvalue in templates), forwarding references, value-category-aware deduction | ✅ |
| Move syntax (`std::move`/`std::forward` spelled-out forms in overload selection) | ✅ |
| `using` aliases and alias templates (east-cv targets included) | ✅ |
| Inline namespaces (`std::__1`, `__cxx11` — descent and mangling) | ✅ |
| `u8`/`u`/`U` literals, UCN escapes, binary literals, digit separators | ✅ |
| `<=>` three-way comparison with rewritten candidates | ✅ |
| C++20 conditional `explicit`, abbreviated function templates (member form) | ✅ |
| `if constexpr` (through real libc++ `<format>` internals) | ✅ |
| Concepts | `template <Concept T>` heads parse as constrained type parameters; constraints are not yet enforced |
| Coroutines, modules | ❌ not supported |

## Known boundaries

- Concept constraints parse but do not gate overload resolution yet
- Coroutines and modules are unsupported
- Multi-return (`q, r := f()`) is a madc-dialect feature, numeric types
  only — not a C++ feature
- The construction-vtables edge case (virtual dispatch *during* base-class
  construction of a further-derived object) uses madc's hidden-parameter
  equivalent; exotic mid-construction dispatch shapes may diverge

When madc's behavior differs from `g++`/`clang++` on a supported feature,
that is a bug — both compilers are the project's canon, and every fix
ships with a reducer test verified against them.
