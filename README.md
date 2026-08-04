# madc — Mad-C Programming Language

**My Advanced Dialect of C** — a C-like scripting language built around the **MC11-IR**: a `cir_node` AST tree that *derives from* [c2mir](https://github.com/vnmakarov/mir)'s `node_t`, so it feeds c2mir → MIR and executes in-process — no bytecode, no separate compilation step. The same tree also carries its originating tokens, so it renders back to source (`.c` / `.mc11`) and forward to other targets from one representation.

The "Mad" in Mad-C: mix functions from multiple programming languages in a single program.

> **Contributing / using an AI agent?** Start with [`AGENTS.md`](AGENTS.md)
> — it's the canonical briefing for every agent (Claude Code, Codex CLI,
> Gemini CLI, Copilot, Cursor, Aider, Windsurf). Rules are in
> [`.claude/rules/`](.claude/rules/), with reasoning in
> [`docs/rules/`](docs/rules/). Cross-agent session flow lives in
> [`docs/agent-handoff.md`](docs/agent-handoff.md).

---

## Quick Start

```bash
# Build
make -C src

# Run a program
bin/madc tests/testint.mad

# Run with debug trace
bin/madc -v tests/testint.mad

# Build first, then run a command against the fresh binary
scripts/build_then.sh bin/madc tests/testint.mad
```

Optional storage backends are now being wired for configure-time
feature detection. When Autotools is installed, the intended flow is:

```bash
autoreconf -fi
./configure --with-bdb --with-gdbm --with-qdbm --with-sqlite3
make
```

`--with-qdbm` is intended for the Villa API layer, and each backend is
optional rather than required for a core `madc` build.

## Multi-file Projects

madc builds multi-file projects from a standard **`compile_commands.json`**
compilation database (the format CMake emits with
`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, or `bear -- make` generates from any
Makefile build): each translation unit compiles separately, the modules are
linked in-process, and the entry point runs.

```bash
bin/madc --project compile_commands.json          # compile all TUs, link, run
bin/madc compile_commands.json                    # .json implies --project
bin/madc --project compile_commands.json -lcrypt  # resolve libs at link time
bin/madc --project compile_commands.json -o app   # single native ELF from all TUs
```

This is how the flagship test case runs: SMAUG 1.8 (~158k lines of C89,
51 translation units) boots both as a multi-TU JIT run and as a single
~5 MB native executable.

For small projects, `#include` composition still works — one top-level file
that `#include`s the rest, with `int main()` last, run as a single
translation unit. Filenames resolve relative to the including file, nested
includes are supported, and repeated includes are skipped within the same
compile.

---

## Language Features

- **Data types:** `int8_t`–`int64_t`, `uint8_t`–`uint64_t`, `float`, `double`, `char`, `std::string`, `array`
- **Typed containers:** `vector<T>`, `map<K,V>`, `set<T>`, `list<T>` — real template instantiations (also as `std::vector<T>` etc.)
- **Streams:** `std::cout`, `std::cerr`, `std::cin`, `std::stringstream`, `std::ifstream`, `std::ofstream`, `std::fstream`
- **Control flow:** `if`/`else`, `for`, `while`, `do`/`while`, `switch`/`case`/`default`, `rust::match`
- **Range-based for:** `for (string name : names) { ... }` — works with array and vector
- **Ternary operator:** `condition ? true_expr : false_expr`
- **Functions:** user-defined with return values and parameters
- **Multiple return values:** `return q, r;` and `q, r := divide(17, 5);` (Go-style)
- **Function pointers:** `auto fn = my_func; fn(args);`
- **Lambdas:** `[](int a, int b) { return a + b; }` with `[&]` capture by reference
- **`defer`:** Go-style deferred execution at scope exit (LIFO order)
- **`auto` keyword:** type inference for function pointer and lambda declarations
- **`:=` short declaration:** `x := 42;` with type inference from RHS
- **`register` keyword:** explicitly register-only variables (never written to memory)
- **User-defined structs:** `struct Point { int x; int y; };`
- **Classes:** the full C++ class model — see **C++ support** below
- **Namespaces:** `std::cout`, `madc::regex_match()`, `php::explode()`, `perl::grep()`, `python::title()`, `ruby::tr()`, `js::btoa()`, `rust::trim()`
- **Dialect precedence:** `prefer rust, php, c;` or `#pragma prefer rust, php, c`
- **Regex:** `madc::regex_match()`, `madc::regex_search()`, `madc::regex_replace()`
- **Input:** `std::cin >> name >> age;` reads from stdin
- **`#include`:** `#include "file.mad"` for source inclusion
- **`using`:** `using namespace std;` or `using std::cout;` imports std names into the unqualified surface
- **`#load`:** `#load "libfoo.so" as foo;` for dynamic library loading
- **`dlopen`/`dlsym`/`dlcall`:** first-class dynamic linking
- **File I/O:** `ifstream`/`ofstream` with `open`, `close`, `good`, `eof`, `getline`
- **Subscript operator:** `a[0]`, `nums[i]`, `ages["key"]`
- **Escape sequences:** `\n`, `\t`, `\r`, `\\`, `\"`, `\0`
- **C23 coverage (early wave):** `_Bool`, `0b...`, `_Static_assert` / `static_assert`, `alignof` / `_Alignof`, `typeof` / `typeof_unqual`, `nullptr`, digit separators (`1'000'000`)

### C++ support

madc is a C/C++ dialect: C++ lowers Cfront-style to strict C11 on the
one `cir_node` IR (`--emit=c11` is a first-class output), and madc
parses the **real glibc/libstdc++ headers** — `#include <vector>`,
`<string>`, `<iostream>` read the actual installed headers, not
embedded shims. The current model (all suite-tested):

- **Classes:** constructors/destructors with RAII (destructor injection
  at every scope exit, return, and unwind path), const methods, static
  members, access control, `friend`, conversion operators, and operator
  overloading — including `<=>` three-way comparison with the standard
  synthesis of the relational operators
- **Inheritance:** single, multiple, and virtual inheritance on the
  Itanium model (vtables, vtable thunks, virtual bases constructed
  once), virtual and pure-virtual methods, virtual destructors, RTTI —
  `typeid` and `dynamic_cast`, including cross-casts in MI hierarchies
- **Templates:** class, function, member, and variable templates via
  real monomorphization on the parse-once spine (the g++ tsubst model:
  the pattern parses once, instantiations substitute over the saved
  tree — never re-parsed); `std::move`/`std::forward` instantiate from
  the real headers
- **`std::string` and streams are real libstdc++ objects**, called
  mangled-direct through Itanium symbols — no wrapper layer
- **Exceptions:** `try`/`catch`/`throw` lowered to setjmp/longjmp with
  type-dispatched catch; destructors run during unwinding
- **Lambdas** with `[&]` capture; references (including rvalue
  references in template instantiation)
- **Specifiers:** `constexpr` (folded; `if constexpr` discards the dead
  branch), `consteval`/`constinit` (accepted), `inline` with real C++
  vague linkage — identical per-TU copies (template instantiations,
  inline/in-class bodies, vtables, typeinfo, C++17 inline variables)
  emit `STB_WEAK` and merge at native or external links, with inline
  variables' dynamic inits once-guarded — plus `inline namespace`,
  `alignas`, `noexcept`, `auto` return deduction
- Version-gated via `--std=`: each keyword activates at its introducing
  standard through the language-standard registry

### Multi-Language Namespaces

The signature feature of madc — use the best functions from each language:

```c
#!/usr/bin/env madc
#include <iostream>
#include <string>
using namespace std;

int main()
{
    // PHP-style string splitting and joining
    string csv = "alice,bob,charlie";
    string delim = ",";
    array names;
    php::explode(names, delim, csv);
    php::sort(names);
    string sorted;
    php::implode(sorted, delim, names);
    cout << sorted << endl;             // alice,bob,charlie

    // Perl-style regex grep
    array matches;
    string pat = "^a";
    perl::grep(matches, pat, names);    // apple, avocado

    // Python-style string formatting
    string title = "hello world";
    python::title(title);
    cout << title << endl;              // Hello World

    // Ruby-style string transforms
    string s = "aabbccdd";
    ruby::squeeze(s);                   // "abcd"

    // JavaScript base64
    string encoded;
    js::btoa(encoded, s);
    cout << encoded << endl;            // YWJjZA==

    // Regex
    int m = madc::regex_match(s, "[a-d]+");
    cout << m << endl;                  // 1

    return 0;
}
```

### Available Namespaces

| Namespace | Functions | Focus |
|-----------|-----------|-------|
| [`php::`](docs/language/ns-php.md) | 36 | String manipulation, array operations (explode, implode, sort) |
| [`perl::`](docs/language/ns-perl.md) | 21 | chop/chomp, grep (regex), glob, split (regex)/join, array ops |
| [`python::`](docs/language/ns-python.md) | 16 | Title case, alignment (center/ljust/rjust/zfill), format |
| [`ruby::`](docs/language/ns-ruby.md) | 12 | squeeze, tr (transliterate), chars, rotate, compact |
| [`js::`](docs/language/ns-js.md) | 6 | Base64 (btoa/atob), URL encoding, parseInt, JSON stringify |
| [`rust::`](docs/language/ns-rust.md) | 18 | trim/contains/replace, split/join, first/last/get, push/pop |
| `std::` | 9 + types | cin, cout, cerr, endl, getline, string conversions, for_each, stream/string types |
| `madc::` | 4 | array, regex_match, regex_search, regex_replace |

Plus `#load` for any shared library via dlopen.

---

## Building

Requires:
- `clang++` (or `g++`) with C++11 support
- The **madc MIR fork** — [github.com/derekbsnider/mir](https://github.com/derekbsnider/mir)
  (branch **`develop`**, pinned at the commit in [`MIR_COMMIT`](MIR_COMMIT) —
  that file is the single source of truth), built at `/workspace/mir`. madc
  links `libmir.a` + c2mir from there. This is **not** upstream MIR: the fork
  carries native C99 `_Complex`, `__attribute__((cleanup))`, ≤16-byte
  SIMD/vector support, direct ELF emission + DWARF + PIC, and the ABI/codegen
  fixes the CIR backend depends on. The fork's `develop` tracks madc's
  `develop` and its `master` tracks madc's `master`; fork releases are tagged
  `v<upstream-base>-madc.<madc-version>` (the release madc depends on is named
  in [`MIR_VERSION`](MIR_VERSION)).

```bash
# Build the MIR fork first (libmir + c2mir):
git clone -b develop https://github.com/derekbsnider/mir /workspace/mir
git -C /workspace/mir checkout "$(cat MIR_COMMIT)"   # pin to the verified commit
make -C /workspace/mir

make -C src           # build bin/madc
make -C src clean     # clean objects
make -C src test      # run unit tests
```

---

## Testing

```bash
# Run unit + integration tests
make -C src fulltest

# Build first, then run one integration test through the batch runner
scripts/build_then.sh bash scripts/run_tests.sh tests/testint.mad
```

**Current status (v0.67.0 plus unreleased parity work): 933 integration tests pass (0 failing, 0 timed out, 9 skipped) through the live binary. A second stdlib flavor is a first-class test lane: `run_tests.sh --stdlib=libc++` runs the whole suite under libc++, with the failing set banked in [`docs/parity/libcxx-failset.txt`](docs/parity/libcxx-failset.txt) (898 passed / 26 failed / 0 timed out at the last whole-lane measurement and burning down; eligible `--exe` and `--obj` cases are each 882/0 — behavior-parity with libstdc++ is the goal, and every remaining failure carries a named root cause). Mach-O targets have a full `.o` lane: `madc -c` writes a real `MH_OBJECT` that `ld64` links and madc reads back. With `-static-libmadc` an emitted binary carries the madc runtime it needs and runs with no madc library installed. gcc.c-torture stands at 1614/1685 with zero standard-C failures — every remaining failure is a classified GNU-extension roadmap item ([`docs/parity/failset-classification.md`](docs/parity/failset-classification.md)). SMAUG 1.8 boots, runs as a live server, and is playable — both as a multi-TU JIT run and as a single native ELF. `cir_node → c2mir → MIR → JIT` is the sole backend (built against the [madc MIR fork](https://github.com/derekbsnider/mir)). (`make -C src fulltest`)**

(`testcin.mad` and `testargv.mad` are driven by `scripts/run_tests.sh` — it
feeds them stdin and argv respectively and asserts on their output.)

---

## Documentation

| Doc | Contents |
|-----|----------|
| [`docs/usage.md`](docs/usage.md) | Language reference, CLI flags |
| [`docs/language/ns-php.md`](docs/language/ns-php.md) | php:: namespace reference |
| [`docs/language/ns-perl.md`](docs/language/ns-perl.md) | perl:: namespace reference |
| [`docs/language/ns-python.md`](docs/language/ns-python.md) | python:: namespace reference |
| [`docs/language/ns-ruby.md`](docs/language/ns-ruby.md) | ruby:: namespace reference |
| [`docs/language/ns-js.md`](docs/language/ns-js.md) | js:: namespace reference |
| [`docs/language/ns-rust.md`](docs/language/ns-rust.md) | rust:: namespace reference |
| [`docs/language/prefer.md`](docs/language/prefer.md) | Namespace precedence directive |
| [`docs/language/modern/`](docs/language/modern/) | Range-for, function pointers, lambdas, defer |
| [`docs/language/switch.md`](docs/language/switch.md) | Switch/case/default statement |
| [`docs/language/rust-match.md`](docs/language/rust-match.md) | `rust::match` (integer patterns, OR-arms, `_` wildcard) |
| [`docs/language/input-operator.md`](docs/language/input-operator.md) | cin >> input operator |
| [`docs/language/class-methods.md`](docs/language/class-methods.md) | Class methods with this pointer |
| [`docs/language/regex.md`](docs/language/regex.md) | Regex functions |
| [`docs/language/multiple-returns.md`](docs/language/multiple-returns.md) | Go-style multiple return values |
| [`docs/language/ternary-operator.md`](docs/language/ternary-operator.md) | Ternary operator |
| [`docs/build.md`](docs/build.md) | Build requirements + the madc MIR fork |
| [`docs/plans/data-storage-federation.md`](docs/plans/data-storage-federation.md) | Exploratory `madcdat` storage/federation design (`madc::DataSource` stays core, `DataSet<T>`, `Relation<A,B>`, automatic mapping, SQL/GQL front-ends, current `--enable-madcdat` build gate, future separate `libmadcdat` boundary) |
| [`docs/architecture.md`](docs/architecture.md) | Compiler internals |
| [`docs/testing.md`](docs/testing.md) | Test guide |
| [`docs/test-status.md`](docs/test-status.md) | Per-test results |
| [`docs/agent-handoff.md`](docs/agent-handoff.md) | Cross-agent hand-off workflow and source-of-truth rules |
| [`AGENTS.md`](AGENTS.md) | Agent briefing — project rules, architecture, multi-tool setup |
| [`docs/rules/`](docs/rules/) | Reasoning behind each rule in `.claude/rules/` |
| [`CHANGELOG.md`](CHANGELOG.md) | Change history |

---

## Current Release

**v0.67.0** — **the flavor-ABI release.** The libc++ behavior-parity
lane went 859/40 → **880/26+2**, zero broken at every comm-diffed
step, and the biggest remaining dam fell: a libc++ script now passes
`std::string` into the host's libstdc++-built namespace functions
(php::, perl::, madc:: eval) through compiler-generated marshalling
thunks (task #69) — host-flavor temps built via the exported string
ctor from the script string's `c_str()`/`size()`, copy-back via the
script flavor's `assign`, alias-mapped reference returns, and honest
boundary detection (dladdr module-origin equality: a symbol
implemented by libc++ itself never marshals). That closed a SILENT
corruption class too — extern-C twins taking `std::string*` had been
fed raw libc++ string bytes and exited 0 with wrong values. Also:
declaration-only Itanium callees get typed protos (#92); a deduction
guide declares NO name ([temp.deduct.guide], #98); #93 typedef
template-arg identity LANDED (`cin >> string` works under libc++);
const-overload selection honors the implicit object parameter
([over.match.funcs]/4 — was silently wrong in BOTH flavors); two
[dcl.ambig.res] declaration readings; access control judges the
SELECTED overload. Suite 902 → 911 (fulltest **911/0/0/9**, `--exe`
**894/0**, `--obj` **894/0**). Fork unchanged (**1.0-madc.0.63.0**).

Previous (v0.66.0) — **the recon-then-strike release.** The lane went
811/77 → 859/40: the 28-test `cout << std::string` bucket fell to ONE
two-commit root (task #88); two-layer SFINAE viability felled the
`allocator_traits::destroy` wall; EIGHT parallel recon agents bucketed
every remaining failure against three-way madc/g++/clang++ reducers
and a five-fix strike batch took 19 more out. Trait-fold silent wrong
answers fixed in BOTH flavors. Suite 889 → 902. Fork unchanged
(**1.0-madc.0.63.0**).

**Branch state:** `develop` is at v0.67.0; `master` is at v0.48.0
(promoted 2026-07-25 — the Mach-O milestone promote, per the owner's
ride-with-S3 decision). The
[MIR fork](https://github.com/derekbsnider/mir)'s `master` tracks
madc's `master` in lockstep (still at the `1.0-madc.0.47.0` pin);
fork releases pair with madc's (see [`MIR_VERSION`](MIR_VERSION)).

### Recent Releases

- **v0.67.0** — the flavor-ABI release: flavored lane 859/40 → 880/26+2 (21 net flips, zero broken at every comm-diffed step); the biggest dam fell — compiler-generated marshalling thunks let a libc++ script call the host's libstdc++-built namespace publics (task #69: thunk per boundary callee at `call_emit_symbol`, dladdr-origin boundary detection, host-flavor temps + `assign` copy-back + alias-mapped returns, `MADC_FLVMAR=0` escape hatch), killing both the loud `NSt3__1` undefined imports AND the silent extern-C `std::string*` corruption; #92 typed protos for declaration-only Itanium callees (the `_Znwm` implicit-int family); #98 a deduction guide declares NO name ([temp.deduct.guide]; gate `testdeductionguide`); #93 typedef template-arg identity landed ([temp.type] — `cin >> string` under libc++; gates `testtypedefarg` + `testcinstr_libcxx`); const-overload selection joins the implicit object parameter ([over.match.funcs]/4; gate `testconstovl`); #94 half (slot arm dark; gate `testpacktypedef`); session #44's [dcl.ambig.res] pair + SELECTED-overload access control (gates `testarrayparam`, `testclassproto`, `testconstaccess`, `testprivmethod`); suite 902 → 911; fork unchanged (1.0-madc.0.63.0)
- **v0.66.0** — the recon-then-strike release: flavored lane 811/77 → 859/40, zero broken at every comm-diffed step; the 28-test `cout << std::string` bucket fell to one two-commit root (a fn-template instantiation outranks ITS OWN varargs placeholder, task #88; gates `testpatcollateral` + `testcoutstr_libcxx`); two-layer SFINAE viability ([conv.ptr] pointer-argument scoring + unevaluated-operand miss is a SFINAE failure; gate `testptrviab`) felled the `allocator_traits::destroy` wall; eight parallel recon agents bucketed all 59 then-remaining failures with three-way madc/g++/clang++ reducers, and the five-fix strike batch took 19 out ([dcl.enum]p11 `testenumqual`, `<=>` payload discovery `testspaceship_libcxx`, per-OVERLOAD memo keys `testosmixed_libcxx`, pointer-param viability/#90 `teststrret_libcxx`, [expr.ref]p4 `testdotstatic`); trait-fold silent wrong answers fixed both flavors (`__has_trivial_destructor(T&)` inverted); the battery caught + fixed its own memo-key regression ([vecbind]); all 40 remaining failures carry named roots; suite 889 → 902 (fulltest 902/0/0/9, `--exe` 886/0, `--obj` 886/0, packed 902/0/0/9); fork unchanged (1.0-madc.0.63.0)
- **v0.65.0** — the VTT wall fell: libc++ `istringstream` RUNS — hidden `__madc_vb<i>` ctor params carry the TRUE virtual-base addresses (madc's construction-vtables equivalent; ONE predicate keys all four signature surfaces; gate `testvttinit`); the three-link stream chain (declaration shells no longer shadow attached out-of-line definitions — `basic_ios::init` had emitted EMPTY; base-subobject destruction uses the D2 flavor via the new `class_base_dtor_symbol` resolver — the external D1 double-destroyed vbases; base-subobject construction demotes the external C1 to the madc C2 body — the library C1 built a standalone layout; gate `testistream_libcxx`); dtor synthesis gates per-symbol on library D1 availability; flavored lane 803/80 → 811/77 (3 fixed, zero broken; `teststreambool` byte-identical under both flavors); suite 887 → 889; fork unchanged (1.0-madc.0.63.0)
- **v0.64.1** — patch: virtual-base default-construction selects its ctor by overload resolution (flavor-independent wrong-codegen in v0.64.0: an unnamed vbase whose class declares a non-default ctor first called the wrong overload with no args; broke every libc++ istringstream default-construction; gate `testvbasedefault`); completeness-on-demand for pending forward instantiations (libc++ `<iosfwd>` stream typedefs: declarations mis-parsed as function decls, `sizeof(istringstream)` silently 0; new `complete_class_type_on_demand` at [basic.def]p5 + [expr.sizeof]p1 consumers; gate `teststreamdecl_libcxx`); suite 885 → 887; fork unchanged (1.0-madc.0.63.0)
- **v0.64.0** — the four-root string/stream breakthrough: flavored lane 747/108 → 803/80 (23 flips in ONE commit, zero regressions at every set-diffed step); the `testclass` SIGSEGV = four stacked defects (unadjusted vbase reference args poisoning every stream test, cast-to-reference ctor args scored as `Tag*` + the binding twin, value-init mem-init on plain-struct members emitted nothing, untyped facet downcast); the 12-link detect-idiom chain (`iterator_traits<CLASS>` resolves, `__is_convertible`, east-specifiers, `__libcpp_datasizeof`, **`vector<int>::push_back` RUNS**); the `__tree`/`<map>` frontier stack (full-spec instantiation keys, `->` through reference-to-pointer, injected-class-name in struct bodies, templated converting ctors); [temp.param]p10 default accumulation both orders (input-stream gateway open); object mode builds the JIT's tree (flavor runtime pre-tree-build); 15 new gates + the probe battery; suite 856 → 885; fulltest 885/0/0/9, `--exe` 869/0, `--obj` 869/0, packed 885/0/0/9; fork unchanged (1.0-madc.0.63.0)
