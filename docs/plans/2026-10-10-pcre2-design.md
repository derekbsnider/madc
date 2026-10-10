# PCRE2, madc's one regex engine (split arc 1A.3)

Phase 1A.3 of `2026-10-09-madc-madcide-split-arc.md`. The decision is the
owner's (2026-10-09, the arc plan's "Regex: PCRE2" entry): PCRE2 is madc's ONE
regex engine, linked into libmadc, a dependency and never vendored. This doc
settles the design items that entry left open and orders the commits. 1B.6's
ex core (`:s`, `:g`, `:v`, `/re/`, `?re?`) is the next consumer.

## 1. What exists (2026-10-10)

- **One engine today: the C++ library's `std::regex`** (ECMAScript grammar),
  in `src/ns_perl.cpp` only, behind five functions. Each call compiles its
  pattern again. libstdc++ and libc++ differ, and libstdc++'s matcher
  recurses per character.

| Function | Today | On an invalid pattern |
|---|---|---|
| `madc::regex_match(s, p)` | whole-string match, `int64_t` | answers 0, silently |
| `madc::regex_search(s, p)` | match anywhere, `int64_t` | answers 0, silently |
| `madc::regex_replace(out, s, p, r)` | every match replaced, `out` a `std::string *` | `out` = `s`, silently |
| `perl::grep(dest, p, src)` | the elements `p` matches | falls back to a SUBSTRING search |
| `perl::split(arr, p, s)` | the fields between matches | falls back to a LITERAL delimiter |

- The `madc::` three are legacy builtins: `extern "C"` `__madc_regex_*` in
  `ns_perl.cpp`, registered by `addFunction` in `src/parser.cpp` (~29255)
  with `std::string`-object parameters. No lean form; `<ns_madc>` declares
  none of them.
- Tests: `tests/testregex.mad` (all five), `tests/testperl.mad`.
- Dependency precedents: zstd (Linux: the distro's shared library, which
  `dpkg-shlibdeps` names in the .deb; macOS and Windows: a pinned static
  build staged on the container, `/workspace/zstd`, with its license under
  `THIRD_PARTY_NOTICES/`), and cmark-gfm (`scripts/stage_cmark_gfm.sh`: a
  pinned per-target static build through CMake, the target's hosted MODE's
  own toolchain). PCRE2 10.42 (`libpcre2-8`) is installed on the container.

## 2. Decisions

1. **Linking.** Linux links the distribution's `libpcre2-8` (shared;
   `pkg-config libpcre2-8`, detected by `configure` like zstd, required:
   `configure` refuses without it). The .deb's Depends gains
   `libpcre2-8-0` through `dpkg-shlibdeps` with no edit; every Ubuntu has it
   installed (`libselinux1` depends on it). Homebrew: `depends_on "pcre2"`. macOS
   (both arches) and Windows link a PINNED static PCRE2 staged per target by
   a new `scripts/stage_pcre2.sh` (the `stage_cmark_gfm.sh` shape: the tag
   in one place, the target's hosted MODE's compiler through
   `stage_cmake_args.sh`, static only, no programs or tests built). The
   pin is PCRE2 10.42, the Linux build host's version, so every platform
   runs one engine version.
2. **No `pcre2_*` export from libmadc.** On Linux the shared dependency keeps
   them out; `check-c-abi-surface.sh` (ELF) proves it, and never by
   `--exclude-libs`. The dylib and DLL surfaces follow zstd's precedent
   (ADR 0003: those lanes are their own).
3. **No JIT.** madc compiles a pattern once per call or per ex command;
   PCRE2's interpreter is enough, and a JIT means W^X pages beside MIR's on
   macOS arm64. The static stages build with `PCRE2_SUPPORT_JIT=OFF` (about
   380 KB of code instead of 610 KB); the Linux shared library carries a JIT
   that madc never calls.
4. **UTF-8.** Patterns and subjects are UTF-8, as every madc text is:
   `PCRE2_UTF | PCRE2_MATCH_INVALID_UTF` (10.34+), so `.` is a code point and
   invalid bytes never fault — they cannot match a UTF construct. A change
   from `std::regex`, which matched bytes.
5. **Syntax.** PCRE2's own (Perl / PHP): unescaped `+ ? | ( )`, `\d`, `\b`,
   lazy quantifiers, lookaround — the modern extended form D24 asks for.
   Replacement text: PCRE2's (`$0`, `$1`, `${1}`, `$$`), through
   `pcre2_substitute` with `PCRE2_SUBSTITUTE_GLOBAL`. ECMAScript's `$&`,
   `` $` `` and `$'` are gone (`$0` is the whole match). 1B.6 maps vim's
   spellings (`\(` `\)`, `\{n,m}`, `&` in a replacement) onto this at the ex
   parser, never in the engine.
6. **An invalid pattern is refused, never answered.** One line on stderr
   naming the function, the pattern, PCRE2's message and its offset —
   `madc::regex_search: invalid pattern "a(b": missing closing parenthesis
   at offset 3` (PHP's `preg_*` warning shape, the `ns_common` stderr
   convention) — and a neutral answer: `false` from the matchers, `out` = the
   subject from the replace, an EMPTY array from grep and split. No fallback
   that pretends the pattern was text.
7. **One owner.** `include/madc_regex.h` / `src/madc_regex.cpp`: a C++ class
   `madc::regex` — compile once (`regex(pattern, flags)`; `ok()`,
   `error()` with message and offset), then `match` (anchored at both ends),
   `search`, `replace`, `split`, `each` (every match's offsets, for
   `perl::grep`, 1B.6's `:g` and `:s`). Every regex in madc goes through it;
   nothing else includes `pcre2.h` (a gate: `check-one-regex-engine.sh`, with
   its negative control, fails on `pcre2.h` or `<regex>` anywhere else).
8. **The `madc::` three move to `src/ns_madc.cpp`, lean and mangled-direct**
   (`cpp-first-api.md`, `dialect-lean.md`). `<ns_madc>` declares the PRIMARY
   forms: `bool regex_match(const char *s, const char *pattern)`, `bool
   regex_search(...)`, `value &regex_replace(value &out, const char *s,
   const char *pattern, const char *replacement)`. The `std::string` forms
   (`testregex.mad`'s `string` shape) are declared inside the stdlib guard
   `#if defined(_GLIBCXX_STRING) || defined(_LIBCPP_STRING)` and implemented
   in the host as real functions over the same owner — no header bodies. The
   `addFunction` registrations in `parser.cpp` and the extern-C
   `__madc_regex_*` are deleted.
9. **`perl::split` follows Perl.** Fields between matches; a leading empty
   field kept; TRAILING empty fields removed (Perl's default limit); an
   empty pattern splits into characters (code points). `perl::grep` keeps
   the elements the pattern matches anywhere.

## 3. Commits (each with its reducer and Tier 1 + Tier 2)

1. **Build + the owner**: `configure` / `src/Makefile` (Linux pkg-config;
   the darwin and Windows MODEs' stage paths), `scripts/stage_pcre2.sh`,
   `madc_regex.{h,cpp}` with unit tests (`tests/unit/test_regex.cpp`: compile
   errors with offsets, anchored match, UTF-8 code points, invalid UTF-8,
   substitute, split), the Homebrew formula, `packaging/notices.tsv` (macOS
   and Windows `libmadc` carry `pcre2-LICENCE` — PCRE2's file name), both
   release packagers' notice copy, the one-engine gate. Platform builds
   proven: `win`, `release-macos` (both arches) on the container.
2. **The five on the owner, refusing invalid patterns**: `perl::grep` /
   `perl::split` in `ns_perl.cpp`; the `madc::` three in `ns_madc.cpp` with
   their lean forms; `parser.cpp`'s registrations and the extern-C names
   gone. Reducer `tests/testregex_refuse.mad` (+ `.expect`): each function
   on an invalid pattern — its stderr line and its neutral answer; the
   oracle is PHP's `preg_match` / `preg_replace` / `preg_split` on the same
   patterns (`php -r`), recorded in the test's comment. `testregex.mad`
   stays green unchanged (its patterns mean the same under PCRE2).
3. **Docs + mirrors**: the regex section of the language docs (syntax, the
   replacement form, UTF-8, refusal), ROADMAP / CHANGELOG / status.

## 4. Thread safety

A `madc::regex` is immutable after construction: concurrent matches on one
object are safe (each match makes its own `pcre2_match_data`); distinct
objects are independent. No global state — no shared compile context or
cache (a pattern cache, if ever measured worth it, is per-object or
per-context, never a global).
