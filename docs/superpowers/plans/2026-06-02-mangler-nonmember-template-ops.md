# Mangler Completeness, Part 2: Non-member std Template Operators (getline/endl/operator<</>>) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Extend `madc_mangle` to generate the **non-member std function-template** symbols
(`std::operator<<`/`>>`, `std::getline`, `std::endl`) — the `_ZSt…I…E<ret><params>` form with
explicit template args, an encoded return type, and template parameters (`T_`/`T0_`/…) — so the
last hardcoded `_ZSt…` stream literals in `cir_builder.cpp` can be generated.

**Architecture:** Part 2 of the mangler-completeness work (W1) in
`docs/superpowers/specs/2026-06-02-retire-std-hardcoding-design.md`. Mangler-only; **no madc
codegen change** (the literals are removed in the later stream-codegen plan). TDD: the six exact
g++ symbols are the oracle — implement and iterate the substitution/template-param logic until the
doctest matches byte-for-byte. Rule #1: every expected symbol came from `g++ -std=gnu++11 -O0 -S`
on this host and is confirmed with `c++filt`.

**Tech Stack:** C++11; `src/madc_mangle.{h,cpp}`; doctest (`tests/unit/test_mangle.cpp`); `g++` + `c++filt`.

---

## The oracle (six symbols, captured from g++ on this host)

```
o << "lit"  std::operator<< <char_traits<char>>(ostream&, const char*)
            _ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc
o << 'c'    std::operator<< <char_traits<char>>(ostream&, char)
            _ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c
o << str    std::operator<< <char,char_traits<char>,allocator<char>>(ostream&, const string&)
            _ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKNSt7__cxx1112basic_stringIS4_S5_T1_EE
o << endl   std::endl<char,char_traits<char>>(ostream&)
            _ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_
in >> str   std::operator>> <char,char_traits<char>,allocator<char>>(istream&, string&)
            _ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE
getline     std::getline<char,char_traits<char>,allocator<char>>(istream&, string&)
            _ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE
```

Decomposition (the `<<` const-char* case): `_ZSt` + `ls` + `I St11char_traitsIcE E` (explicit
template args) + `RSt13basic_ostreamIcT_E` (return type; `T_`=template-param 0) + `S5_` (param 0 =
return, back-ref) + `PKc` (param 1). Function templates **encode the return type**; members do not.

## Design (the new capability)

- **Template-param placeholder in a type string:** `$T0`→`T_`, `$T1`→`T0_`, `$T2`→`T1_`, … (the
  Itanium `<template-param>` spelling: `T_` for index 0, then `T` + base-36(index-1) + `_`).
  Parsed by `parse_type`; a `<template-param>` IS a substitution candidate (the existing engine's
  candidate table absorbs it, so a later concrete-type back-ref like `S4_` falls out naturally).
- **Free std template function API:**
  ```cpp
  // _ZSt <op-or-srcname> I <targs> E <ret> <params...>   (one substitution table)
  std::string itanium_mangle_std_free_template(
          const std::string &name,                       // "<<" / ">>" / "getline" / "endl"
          const std::vector<std::string> &targs,         // explicit <...> args
          const std::string &ret,                        // return type (may use $Tn)
          const std::vector<std::string> &params);       // param types (may use $Tn)
  ```
  `name` → `operator_code(name)` if non-empty, else `source_name(name)`.

## File Structure

| File | Change |
|------|--------|
| `tests/unit/test_mangle.cpp` | Add a `TEST_SUITE` pinning the six symbols. |
| `include/madc_mangle.h` | Declare `itanium_mangle_std_free_template`. |
| `src/madc_mangle.cpp` | `$Tn` in the type AST/parser/encoder; `ItaniumMangler::mangle_std_free_template`; the public wrapper. |

---

## Task 1: Failing doctest pinning the six oracle symbols

**Files:** Modify `tests/unit/test_mangle.cpp` (append after the complete-spec-abbrev suite)

- [ ] **Step 1: Add the suite**
```cpp
TEST_SUITE("Itanium substitution: non-member std template operators") {
	static const std::string TR = "std::char_traits<char>";
	static const std::string AL = "std::allocator<char>";
	static const std::string STR = std_string_type();          // __cxx11 string

	TEST_CASE("operator<< (ostream&, const char*) / (ostream&, char)") {
		CHECK(itanium_mangle_std_free_template("<<", {TR},
		        "std::basic_ostream<char,$T0>&",
		        {"std::basic_ostream<char,$T0>&", "const char*"})
		      == "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc");
		CHECK(itanium_mangle_std_free_template("<<", {TR},
		        "std::basic_ostream<char,$T0>&",
		        {"std::basic_ostream<char,$T0>&", "char"})
		      == "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c");
	}

	TEST_CASE("operator<< (ostream&, const string&)") {
		CHECK(itanium_mangle_std_free_template("<<", {"char", TR, AL},
		        "std::basic_ostream<$T0,$T1>&",
		        {"std::basic_ostream<$T0,$T1>&", "const " + STR + "&"})
		      == "_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKNSt7__cxx1112basic_stringIS4_S5_T1_EE");
	}

	TEST_CASE("endl") {
		CHECK(itanium_mangle_std_free_template("endl", {"char", TR},
		        "std::basic_ostream<$T0,$T1>&",
		        {"std::basic_ostream<$T0,$T1>&"})
		      == "_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_");
	}

	TEST_CASE("operator>> (istream&, string&) and getline") {
		const char *expect =
		    "Ic St11char_traitsIcE SaIcE E R St13basic_istream I T_ T0_ E S7_ "
		    "R NSt7__cxx1112basic_string I S4_ S5_ T1_ E E";   // (spaced for reading)
		(void)expect;
		CHECK(itanium_mangle_std_free_template(">>", {"char", TR, AL},
		        "std::basic_istream<$T0,$T1>&",
		        {"std::basic_istream<$T0,$T1>&", STR + "&"})
		      == "_ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE");
		CHECK(itanium_mangle_std_free_template("getline", {"char", TR, AL},
		        "std::basic_istream<$T0,$T1>&",
		        {"std::basic_istream<$T0,$T1>&", STR + "&"})
		      == "_ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE");
	}
}
```

- [ ] **Step 2: Declare the API in `include/madc_mangle.h`** (after `itanium_mangle_operator_sub`):
```cpp
// Mangle a non-member std:: function template (operator or named), e.g.
//   std::operator<< <char_traits<char>>(basic_ostream<char,_Traits>&, const char*)
// Form: _ZSt <op-or-source-name> I<targs>E <ret> <params...>. Function templates
// encode the return type. Template parameters in `ret`/`params` are written as
// "$T0","$T1",… (=> T_,T0_,…). `name` is an operator spelling ("<<") or a source
// name ("getline","endl").
std::string itanium_mangle_std_free_template(const std::string &name,
        const std::vector<std::string> &targs,
        const std::string &ret,
        const std::vector<std::string> &params);
```

- [ ] **Step 3: Build + run — verify it fails to link/compile (API not defined yet)**

Run:
```bash
make -C src 2>&1 | tail -3
( ulimit -t 60; g++ -std=c++11 -Iinclude tests/unit/test_mangle.cpp obj/madc_mangle.o -o tmp/test_mangle )
```
Expected: link error `undefined reference to itanium_mangle_std_free_template` — confirms the test
targets the new API.

## Task 2: Implement `$Tn` template-param support in the type AST/encoder

**Files:** Modify `src/madc_mangle.cpp`

- [ ] **Step 1: Add a template-param field to `TypeNode`** (struct `TypeNode`, after `builtin`):
```cpp
	int tparam = -1;                   // >=0 if this type is template-param #N ($Tn)
```

- [ ] **Step 2: Parse `$Tn`** — in `parse_type`, after the decoration-peel loop and before
  `builtin_code`, add:
```cpp
	if (s.size() >= 3 && s[0] == '$' && s[1] == 'T') {
		t.tparam = std::stoi(s.substr(2));
		return t;
	}
```

- [ ] **Step 3: Encode a template-param** — in `encode_core` (and `canon_type_core`), handle it.
  In `encode_core`, before the builtin check:
```cpp
		if (t.tparam >= 0) return tparam_ref(t.tparam);
```
  In `canon_type_core`, before the builtin return:
```cpp
		if (t.tparam >= 0) return "$T" + std::to_string(t.tparam);
```
  Add the helper to `ItaniumMangler` (near `subref`):
```cpp
	static std::string tparam_ref(int n)
	{
		if (n == 0) return "T_";
		int v = n - 1; std::string d; const char *D = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
		if (v == 0) d = "0";
		while (v > 0) { d = std::string(1, D[v % 36]) + d; v /= 36; }
		return "T" + d + "_";
	}
```
  NOTE: whether a `<template-param>` itself becomes a substitution candidate is decided by the
  oracle in Task 3 — if the symbols don't match, adjust `encode_type` to `add_sub("$T"+N)` for a
  bare template-param (the ABI lists `<template-param>` as substitutable).

## Task 3: Implement `mangle_std_free_template` and CONVERGE against the oracle

**Files:** Modify `src/madc_mangle.cpp`

- [ ] **Step 1: Add the method to `ItaniumMangler`** (public):
```cpp
	std::string mangle_std_free_template(const std::string &opOrName,
	        const std::vector<std::string> &targs,
	        const std::string &ret,
	        const std::vector<std::string> &params)
	{
		reset();
		std::string out = "_ZSt" + opOrName;
		out += "I";
		for (const auto &a : targs) out += encode_type(parse_type(a));
		out += "E";
		out += encode_type(parse_type(ret));
		for (const auto &p : params) out += encode_type(parse_type(p));
		return out;
	}
```

- [ ] **Step 2: Add the public wrapper** (after `itanium_mangle_operator_sub`):
```cpp
std::string itanium_mangle_std_free_template(const std::string &name,
        const std::vector<std::string> &targs,
        const std::string &ret,
        const std::vector<std::string> &params)
{
	std::string code = op_special(name);
	std::string opOrName = code.empty() ? source_name(name) : code;
	ItaniumMangler m;
	return m.mangle_std_free_template(opOrName, targs, ret, params);
}
```

- [ ] **Step 3: Build, run, COMPARE to the oracle, ITERATE**

Run (repeat after each adjustment):
```bash
make -C src 2>&1 | tail -3
( ulimit -t 60; g++ -std=c++11 -Iinclude tests/unit/test_mangle.cpp obj/madc_mangle.o -o tmp/test_mangle )
( ulimit -t 60; timeout 60 ./tmp/test_mangle 2>&1 | tail -20 )
```
Compare each FAIL's `got` vs `expect` character-by-character. The substitution/template-param
numbering is the subtle part; adjust ONLY the candidate logic (e.g. whether a bare `<template-param>`
adds a slot, the order candidates accumulate across targs→ret→params) until **all six match
byte-for-byte**. Do NOT special-case a symbol; the fix must be a general rule the engine applies.
Re-confirm any uncertainty directly against g++: `c++filt '<symbol>'`.

Expected end state: all six `CHECK`s pass; all pre-existing suites still pass.

## Task 4: Full gate + commit

- [ ] **Step 1: Full unit suite** — `( ulimit -t 180; timeout 240 make -C src test ) 2>&1 | tail -8` — all pass.
- [ ] **Step 2: Integration safety net** — `make -C src 2>&1 | tail -3` then `( ulimit -t 240; timeout 300 bash scripts/run_tests.sh ) 2>&1 | tail -3` — unchanged **457 / 7 / 55** (mangler-only, inert).
- [ ] **Step 3: Commit**
```bash
git add include/madc_mangle.h src/madc_mangle.cpp tests/unit/test_mangle.cpp
git commit -m "feat(mangle): non-member std template operators (getline/endl/operator<</>>)

Adds itanium_mangle_std_free_template + \$Tn template-param support so the
_ZSt..I..E<ret><params> function-template symbols are GENERATED, not hardcoded.
Six g++-verified symbols pinned in test_mangle (vs c++filt). Mangler-only; no
codegen change (the cir_builder _ZSt literals are removed in the stream-codegen plan).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

- **Spec coverage:** completes W1 (mangler is now the single source of every std:: symbol — member
  ops from Part 1, non-member template ops here). Enables the stream-codegen plan to delete the
  `_ZSt…` literals.
- **Placeholder scan:** the only deliberately-iterative step is Task 3 Step 3 (substitution/`T_`
  numbering converged against the g++ oracle) — that is TDD, not a placeholder; the target symbols
  are fully concrete.
- **Type consistency:** `itanium_mangle_std_free_template(name,targs,ret,params)` declared in the
  header and defined identically; `$Tn`→`tparam_ref` in both `encode_core` and `canon_type_core`.
- **Risk:** substitution/template-param interplay is subtle; mitigated by the six exact-symbol
  doctests as the oracle and the rule "general fix only, never special-case a symbol."
