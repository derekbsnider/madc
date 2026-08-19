# php::array_push as a real OVERLOAD SET — owner ruling, ready to execute

**Status:** DESIGN SETTLED (owner, 2026-08-19), NOT started — deferred past a
compaction. Execute as specified; the decisions below are made.

**Owner ruling (verbatim intent):** PHP has no `array_push_int()`.
`array_push()` allows different types, handled as function overloads:
`php::array_push(array&, int)`, `(array&, char*)`, `(array&, value&)`, etc.

## Motivating defect (measured)

`php::array_push(a, 7)` resolves the ONLY declared overload — `(array&,
const char*)` — passes 7 as the pointer (c2mir warns "using integer without
cast for pointer type parameter"), and basic_string's ctor SIGSEGVs on
address 7. Found writing tests/testforeachvalue.mad; the int spelling today
is `array_push_int`.

## The set

| overload | runtime entry | status |
|---|---|---|
| `array_push(array&, const char*)` | `__php_array_push` | exists |
| `array_push(array&, int64_t)` | `__php_array_push_int` | exists (named alias today) |
| `array_push(array&, double)` | `__php_array_push_real` | NEW |
| `array_push(array&, bool)` | `__php_array_push_bool` | NEW |
| `array_push(array&, array&)` | `__php_array_push_array` | exists (named alias today) |
| `array_push(array&, value&)` | `__php_array_push_value` | NEW — the kind-preserving general push, copies via `value::operator=` exactly like `php_array_get_value` (src/ns_php.cpp:326) does on the read side |
| `array_push(array&, std::string&)` | via c_str -> `__php_array_push` | header convenience, gate like ns_madc's `_GLIBCXX_STRING` block if <ns_php> needs it (check whether <ns_php> already assumes <string>) |

## Governing principle (owner, 2026-08-19 — applies beyond this change)

**php:: functions work as closely to the original PHP functions as possible.
Invented madc-isms (`array_push_int`) are DRIFT, and such deviations require
operator discussion/approval.** This is the design law for the borrowed-
language namespaces; parity questions get answered by php-cli, the same way
the dump arc oracled print_r byte-for-byte.

Two PHP-parity facts to honour in this change (both are current drift):

- **PHP's `array_push` RETURNS the new element count** (int). madc's is
  `void`. The overloads should return `int64_t` = the new count.
- **PHP's `array_push` is variadic** (`array_push($arr, $v1, $v2, ...)`).
  Still recorded as the follow-on (separate mechanism), but it is PARITY
  work, not enhancement.

## Decided per the principle

- **`<ns_php>`'s existing style is inline wrapper bodies over the extern-C
  entries** (`void array_push(array &values, const char *text) {
  __php_array_push(&values, text); }` — include/madc/ns_php:62). ADD THE
  OVERLOADS IN THE SAME STYLE — same-name namespace functions; the
  `namespace_fn_overload_sets` machinery mints per-overload symbols (the
  ns_madc eval set proves same-name ns overloads work). Do NOT convert ns_php
  to mangled-direct in this change (that is the cpp-first-api migration, a
  separate arc).
- **`array_push_int` / `array_push_array` are DRIFT and get RETIRED in this
  same change** (the owner's principle above supersedes the earlier
  keep-as-aliases default). Migrate the tests that use them (testforeach,
  testarraymethods, testvalueintrinsic, testarraysubscript, + grep for the
  rest) to the one PHP-true name. The extern-C `__php_array_push_int` /
  `_array` entries STAY (they are the runtime plumbing the overload wrappers
  call, not script surface).
- **PHP's multi-value form** `array_push($arr, $v1, $v2, ...)` is VARIADIC —
  a separate mechanism (variadic namespace publics), recorded as a follow-on,
  NOT in this change.
- **bool before int:** overload ranking must send a madc `bool` to the bool
  overload, not promote to int64 — verify with the test; if ranking ties,
  drop the bool overload rather than fight ranking (PHP pushes true as bool
  but 1 is observably similar; document whichever lands).

## Gate

`tests/testarraypush.mad`: push one of each type through the ONE name,
`php::print_r` the array (kinds are visible in var_dump output — use
`php::var_dump` for the kind words), assert `a.count()`, keep one
`array_push_int` call as the alias-compat line. The old SIGSEGV shape
(`array_push(a, 7)`) becomes the int-overload line by construction.

## Sequencing note

Landed BEFORE this (same session, 2026-08-19): `for (value v : a)` (commit
50abac02, released v0.87.0) and `cout << value` (see
2026-08-19-range-for-auto-deduction.md §follow-up — the carrier arm in
lower_free_operator_to_call + <ns_madc> gated declaration + host
madc::operator<<). Release cadence: this change ships WITH a release when it
lands.
