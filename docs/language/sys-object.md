# The System Object — `madc::sys`

`madc::sys` follows the Python `sys` convention: one typed object with
dot members holding the program's runtime facts and argument lists.
It lives in the `<ns_madc>` embedded header — auto-included on first
`madc::` use in the default dialect; standards modes (`--std=c*` /
`--std=c++*`) need the explicit `#include <ns_madc>` (the same way
Python requires `import sys`). The same header carries the runtime
eval API — see [eval.md](eval.md).

```cpp
#include <ns_madc>
#include <iostream>
using namespace std;

int main(int argc, char **argv)
{
    cout << "running " << madc::sys.argv[0]
         << " on " << madc::sys.platform
         << " (madc " << madc::sys.version << ")" << endl;
    cout << "args: " << madc::sys.argv.count() << endl;
    return 0;
}
```

(Declare `main` with parameters to receive arguments — see
[All lanes](#all-lanes); script-mode programs get the script path as
`argv[0]` automatically.)

## Members (v1)

| Member | Type | Mutability | Semantics (Python analog) |
|---|---|---|---|
| `sys.argv` | `array` | mutable | `argv[0]` = script path, then arguments (`sys.argv`) |
| `sys.path` | `array` | mutable | search dirs for future `#load`/eval; seeded `[script-dir, "."]` (`sys.path`) |
| `sys.platform` | `const char *` | fact | `"linux"`, `"darwin"`, `"win32"` (`sys.platform`) |
| `sys.version` | `const char *` | fact | the madc version string (`sys.version`) |
| `sys.hostname` | `const char *` | fact | the host's name — madc extension (Python: `socket.gethostname()`) |

- There is **no `sys.argc`** — the array is self-sizing
  (`sys.argv.count()`), and a separate count would silently desync when
  `argv` is mutated. Bare `argc`/`argv` main parameters remain the raw
  C door (see [argc-argv.md](argc-argv.md)).
- `sys.path` has Python's one-way semantics: mutations affect future
  `#load`/eval resolution only — already-processed `#include`s are not
  re-resolved.
- Facts follow Python-level semantics: the values they point at are
  immutable; the members are rebindable pointers (Python allows
  `sys.platform = ...` too). Compile-time rejection of fact assignment
  arrives with const-qualified member support.
- Future members join by appending: `executable`, `env`, cwd, pid, …

## `MADC_VERSION`

`MADC_VERSION` is a preprocessor macro expanding to the build's version
string literal — the compile-time spelling of the same fact
`madc::sys.version` carries at runtime:

```cpp
cout << MADC_VERSION << endl;            // e.g. "0.38.0"
bool same = (madc::sys.version == string(MADC_VERSION));  // true
```

## All lanes

`sys` works identically under the JIT, native artifacts (`-o`/`-c`),
`--emit=c11`, and script mode. The compiler injects
`__madc_sys_init(argc, argv)` at `main` entry (before global
initializers) in any TU that included `<ns_madc>`; a `main` declared
without parameters gets an empty `argv`. Shared objects (`-shared`)
have no `main`, so `argv` stays empty there; the facts are always
populated.

# Array Methods

The builtin `array` is madc's generic polyglot value container — the
`php::`/`perl::`/… namespace functions are language-flavored skins over
the same object. It also carries native methods:

| Method | Returns | Semantics |
|---|---|---|
| `.count()` | `long` | number of elements (PHP spelling) |
| `.size()` | `long` | number of elements (C++/Ruby spelling) |

```cpp
array a;
php::array_push(a, "one");
php::array_push(a, "two");
cout << a.count() << endl;   // 2
cout << a.size() << endl;    // 2
cout << madc::sys.argv.count() << endl;
```

Both spellings are the same operation (an object-kind array reads as
length 0, matching range-for). A fuller native method surface
(`.push()`, `.pop()`, …) is planned alongside the array-as-real-class
retirement.
