# The madc Language — Overview

madc's default dialect is C/C++ with the ceremony removed and a small set
of utility-language extensions. Explicit `--std=c*` / `--std=c++*` modes
keep their standard rules; everything on this page describes the default
`--std=madc` dialect unless noted.

## Script mode

Top-level statements are allowed; madc collects them into a synthesized
`main()`. A file can be a one-liner:

```c
puts("hello");
```

Programs that define their own `main()` work exactly as in C/C++.

## Types

| Type | Description |
|------|-------------|
| `int` / `int64_t` | 64-bit signed integer (**the dialect's default int is 64-bit**) |
| `int8_t` .. `int32_t` | smaller signed integers |
| `uint8_t` .. `uint64_t` | unsigned integers |
| `float`, `double` | floating point |
| `char` | 8-bit character |
| `std::string` | the real C++ `std::string` (`#include <string>`) |
| streams | the real `std::cout`/`cin`, `fstream`, `stringstream` families (`#include <iostream>`, …) |
| `array` | built-in mixed-type array (used by the utility namespaces) |

Under `--std=c*` modes, `int` is the standard 32-bit C `int`.

`struct` and `class` definitions follow C++; class instances need no
`struct`/`class` prefix at declaration. See
[class methods](class-methods.md) and the
[supported C++ features](cpp-features.md) reference.

## Standard library

Everything in libc and the active C++ standard library (libstdc++ by
default, libc++ under `-stdlib=libc++`) is available — include the normal
headers and call. madc parses the real installed headers; `std::string`
and the streams are the real library objects.

## Dialect extensions

| Feature | Doc |
|---------|-----|
| `x := expr` — short variable declaration with type inference | (Go-style; see `tests/testcolon.mad`) |
| UFCS — `x.f(y)` ≡ `f(x, y)`, either spelling | [ufcs.md](ufcs.md) |
| `defer stmt;` — run at scope exit | [modern/defer.md](modern/defer.md) |
| Multiple return values — `return a, b;` / `q, r := f();` | [multiple-returns.md](multiple-returns.md) |
| `rust::match` expression | [rust-match.md](rust-match.md) |
| Lambdas — `[](args) { ... }`, typed return in brackets | [modern/lambdas.md](modern/lambdas.md) |
| Range-based `for` over arrays and containers | [modern/range-for.md](modern/range-for.md) |
| Function pointers via `auto` | [modern/function-pointers.md](modern/function-pointers.md) |
| Built-in regex via `madc::` | [regex.md](regex.md) |
| `sys` object — argv, environment, process info | [sys-object.md](sys-object.md) |
| `madc::channel` — URI byte channels (`exec://`, `tcp://`, `file://`) | [channel.md](channel.md) |

## Namespaces

The multi-language utility namespaces are ordinary libraries under
explicit namespaces — include the matching header, then call qualified:

```c
#include <ns_php>
#include <string>

std::string s = "  hi  ";
php::trim(s);
```

| Header | Namespace |
|--------|-----------|
| `<ns_php>` | [`php::`](ns-php.md) — string + array utilities |
| `<ns_perl>` | [`perl::`](ns-perl.md) — grep, glob, split, chop/chomp |
| `<ns_python>` | [`python::`](ns-python.md) — formatting, alignment |
| `<ns_ruby>` | [`ruby::`](ns-ruby.md) — squeeze, tr, chars, rotate |
| `<ns_js>` | [`js::`](ns-js.md) — base64, URL encoding, JSON |
| `<ns_rust>` | [`rust::`](ns-rust.md) — string/collection utilities, `match` |
| `<ns_madc>` | `madc::` — regex, eval, native madc services |
| `<ns_ui>` | [`ui::`](ns-ui.md) — data-hub worlds, projections, verbs, prompt |

Unqualified calls (`trim(s)` instead of `php::trim(s)`) resolve after a
`using namespace php;` or a [`prefer`](prefer.md) directive, which also
ranks the winner when several namespaces define the same name.

## Control flow

`if`/`else`, `for`, `while`, `do`/`while`, [`switch`](switch.md), and the
[ternary operator](ternary-operator.md) follow C/C++ semantics. Range-based
`for` works over `array` and the C++ containers.

## Program arguments and input

See [argc-argv.md](argc-argv.md) for script arguments and
[input-operator.md](input-operator.md) for `cin >>` input.
