# Value-First madc Code — reasoning

Owner directive (2026-08-20, during the Adventure A5 slice): "in madc,
madc::value is preferred over std::string... avoid std::cout — use
std::print and std::println... we put a whole lot of effort into making
madc::value extra robust, and std::print and std::println and
std::format [are] hard-coded into madc because std::string and iostream
are slow and terrible with way too much overhead."

Owner directive (2026-08-21, the zero-include Adventure rewrite): madc
does auto-including and auto-namespace resolution — a madc program
spelling `#include <...>` walls or `std::` prefixes reads like
"a plain-old-C project", not madc. The intrinsics are bare
`print`/`println` ("why are you prefixing with std::?? madc makes this
unnecessary"), and a `print("...\n")` is a `println` in disguise. The
compiler gaps that once forced the spellings (the module-blind
auto-include scan, the main-file-only prelude insertion, the
using-directive-only std call fallback) were fixed the same day —
which is the rule's mechanism: the SPELLING gap always indicts the
compiler, never the script. The same held on 2026-10-05: the scan read
every angle include as a system header, so a program's own header found
through `-I` (madcide's `<madcide/harness>`) lost the service. The scan
now classifies by the resolved path, as gcc and clang do
(`tests/testautoincludeangle`).

## Why the rule exists

- The value carrier (include/libmadc/value.h + the madarray_* runtime
  family) is the language's own showcase surface — the polyglot
  namespaces (php::, perl::, ...) are skins over it, and the dump
  intrinsics, keyed access, and iteration arcs all invested in its
  robustness. Example code that reaches for std::string first
  advertises the wrong dialect and silently skips dogfooding the
  carrier.
- std::print/std::println/std::format are always-included intrinsics
  with compile-time format validation lowering to the rt_format engine
  (tests/teststdprint*.mad) — dramatically lighter than iostream's
  locale/virtual machinery. cout << exists for C++ compatibility, not
  for madc-dialect code.
- The near-miss that minted the rule: the A5 Adventure game skeleton
  was about to be written with std::string tokenizers and cout-style
  output because the carrier lacked substr/==/case helpers. The right
  move was to ADD the missing string surface to the carrier (the same
  add_array_methods() seam c_str()/count()/as_integer() ride), which
  benefits every future madc program, instead of coding around it once.

## The L3 caveat

Value-by-value returns from script functions (L3 in the Adventure plan)
landed on 2026-09-26: `madc::value` is non-trivial for calls, so a `var`
result is constructed through the hidden result address, the ABI g++
and clang give it. The carrier methods written before it that
conceptually return a NEW value (substr, case transforms) still return
ring-lifetime `const char *` text: safe to pass onward or capture into
a var immediately (the ctor copies), not to store as a raw pointer. They
can gain value-returning overloads without breaking callers.

`c_str()` itself is NOT ring text (owner 2026-10-09, "make it a real
borrow"): it returns the carrier's own NUL-terminated payload, exactly
std::string's contract. As a ring copy it handed every caller a pointer
that silently changed under the ninth ring write on the thread — madcide
held 48 such pointers across text work, and ASan reported
heap-use-after-free in the REPL tests. A borrow is stable for as long as
the carrier lives unmodified, costs no copy, and is the contract C++
readers already assume. The one place a borrow would outlive its carrier
— a `char *` function returning a local carrier's text (`return v;`,
`return v.c_str();`) — is copied out by the compiler at the return
(`__madc_text_escape`, marked by `FuncDef::borrows_receiver_text`), and
assigning a carrier from a pointer into its own text (`v = p + 2`) is
alias-safe. Holding a borrow past the carrier's scope is the same bug it
is with std::string: keep the carrier in the pointer's scope
(`testvarcstrborrow`).

## Why `var`, not `value`, in dialect source (owner rule 2026-08-31)

Under `--std=madc` the parser resolves `var`, `value`, and `array` to the
same carrier type (`madc_dialect_type_spelling` in `src/parser.cpp`), so
the choice is purely a spelling — and the owner has ruled it several
times: dialect code reads as madc, and `var` is madc's word. `array` is
also fine in dialect code where it carries information — a variable
that starts its life as an array (owner refinement, same day). `value`
remains the C++-interop name (embedded-header signatures, engine
source, `madc::value` in C++ hosts), where it is the correct spelling. Mixed spellings in one file made the dialect look like C++
with a library; one spelling makes it a language. The mechanical
conversion is safe by construction (alias), but the repo-wide sweep of
`tests/` waits for a merge wave because only the full battery validates
every touched test.
