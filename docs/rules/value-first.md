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
compiler, never the script.

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

Value-by-value returns from script-visible functions (L3 in the
Adventure plan) are not landed. Carrier methods that conceptually
return a NEW value (substr, case transforms) return ring-lifetime
`const char *` text — the established c_str() contract: safe to pass
onward or capture into a var immediately (the ctor copies), not to
store as a raw pointer. When L3 lands these can gain value-returning
overloads without breaking callers.
