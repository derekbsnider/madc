# Uniform Function Call Syntax (UFCS)

**Available only in the madc dialect (`--std=madc`, the default).** Every
explicit `--std=c*` / `--std=c++*` mode behaves exactly as it did before UFCS
existed.

In madc, a function call can be written either way round:

```c
int twice(int n) { return n * 2; }

int n = 21;
n.twice();      // 42  — same as twice(n)
twice(n);       // 42
```

Neither spelling is preferred by the language; you pick whichever reads better
at the call site. That is the whole feature.

## The rule

Two fallbacks, each tried in one fixed direction. There is no merged overload
set and no ranking between members and free functions.

```
x.f(y)   ->  viable member f?         use it
         ->  otherwise                f(x, y)

f(x, y)  ->  viable declared free f?  use it
         ->  otherwise                x.f(y)
         ->  otherwise                the usual unresolved-symbol handling
```

Member-first for the dot form, free-first for the call form.

**A fallback only ever fires where the code was already an error.** If
`x.f(y)` finds a member, nothing changes. If `f(x, y)` finds a declared free
function, nothing changes. So no program that compiled before UFCS can change
meaning — the feature adds spellings, it never re-points existing ones.

## The receiver is passed exactly as written

No implicit `&`, no implicit `*`, no type changes:

```c
int sum2(int *p) { return p[0] + p[1]; }

int a[2] = { 3, 4 };
int *ip  = a;

a.sum2();       // sum2(a)
ip->sum2();     // sum2(ip)   — NOT sum2(*ip)
```

This makes `.` and `->` the *same* operator in the fallback leg, which is what
lets C pointer APIs read naturally:

```c
FILE *fp = fopen("x", "r");
fp->fclose();   // fclose(fp)
```

`p->f()` and `(*p).f()` remain equivalent for the **member** leg, which is tried
first. The equivalence simply does not extend to the fallback leg — that leg is
a hard error in C++ to begin with.

## Chaining

Because a fallback resolves into an ordinary call, its result chains like any
other call result:

```c
int twice(int n) { return n * 2; }
int inc(int n)   { return n + 1; }

int n = 5;
n.twice().inc().twice();        // twice(inc(twice(5))) == 22
```

Chains may mix members and free functions freely in either order:

```c
b.bump().doubled();     // free bump, then member doubled
b.doubled().twice();    // member doubled, then free twice
```

## Namespace functions become extension methods

The fallback resolves `f` through madc's ordinary unqualified lookup — the same
chain a hand-written `f(x, y)` uses (current namespace → `using` → preference
order → global). No new lookup rule, and no per-namespace special-casing: a
borrowed-language helper reads as a method exactly when it is already visible.

## Containers

Member-only container operations gain the free-function spelling:

```c
#include <map>

std::map<int,int> m;
m[3] = 9;

count(m, 3);    // m.count(3) == 1
```

Note that `size`, `begin`, `end` and `empty` need no help — the standard library
already declares free versions of them, and those keep resolving to the real
`std::` functions. UFCS is what covers the operations the library only exposes
as members.

## What does not participate

- **Operators.** `a.operator+(b)` and friends are untouched.
- **Constructors**, and `.` applied to a namespace.
- **A static member never captures a call.** A static takes no receiver, so
  re-forming `f(x)` as `x.f()` would drop the argument; the fallback declines.
- **Explicit template arguments** on the dot form (`x.f<T>(y)`) are not
  re-formed.
- **A compound first argument** on the call form (`count(get_map(), k)`) is not
  re-formed. Only a plain named variable is recognised as a receiver there.
- **Wrong-arity members do not fall through.** If the receiver's type has *any*
  member spelled `f`, the dot form uses it and reports its own error rather than
  reaching for a free function.

## Diagnostics

When both lookups miss, madc reports one error naming both attempts, so a UFCS
miss never reads like a plain typo:

```
error: Unidentified member 'nosuch' in 'Empty', and no function 'nosuch'
       in scope for the UFCS form 'nosuch(Empty, ...)'
```

## Background

This follows Stroustrup's unified-call proposal (N4174, and the
[2016 background note](https://isocpp.org/blog/2016/02/a-bit-of-background-for-the-unified-call-proposal)):
try the other syntax if the first one failed, with two separate ordered
fallbacks rather than a merged overload set. C++ never adopted it — objections
centred on `x.f(y)` finding `f(x, y)`, since it makes a class's apparent
interface open-ended. madc takes that trade deliberately: the behaviour is
confined to `--std=madc`, and it can only affect code that was already an error.

Tests: `tests/testufcs*.mad`. Gate: `scripts/ufcs_gate.sh` (in `fulltest`)
sweeps the whole `--std=` matrix to keep the feature inside the dialect.
