# Mangler Completeness, Part 1: Complete-Specialization Abbreviations (So/Si/Sd/Ss) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix `madc_mangle` so the standard-library *complete-specialization* abbreviations
`So`/`Si`/`Sd`/`Ss` are emitted standalone (no appended template args), matching the exact symbols
libstdc++ exports — the precise root-cause bug that caused the stream `operator<<` symbols to be
hardcoded as `_ZSt…` literals.

**Architecture:** This is the first, lowest-risk increment of the "retire ALL std:: hardcoding"
campaign (`docs/superpowers/specs/2026-06-02-retire-std-hardcoding-design.md`). It changes ONLY the
mangler + its unit test; **no madc codegen behavior changes** (the `_ZSt…` literals in
`cir_builder.cpp` stay until a later plan switches call-emission to the mangler). Rule #1: every
expected symbol is the real libstdc++ symbol, verifiable with `c++filt` / `nm -D libstdc++`.

**Tech Stack:** C++11; `src/madc_mangle.cpp` (the Itanium mangler); doctest (`tests/unit/test_mangle.cpp`);
`g++` + `c++filt` as the canonical reference.

---

## Background (verified file:line)

- The bug is in `src/madc_mangle.cpp`. `std_abbrev()` (`:501-511`) maps `std::basic_ostream`→`So`,
  `std::basic_istream`→`Si`, `std::basic_iostream`→`Sd`, `std::basic_string`→`Sb`. In
  `encode_name()`'s **template branch** (`:542-572`), an abbreviation is treated as a *template
  prefix* and the args are appended as `I…E`. That is correct for `Sa`/`Sb` (template names) but
  WRONG for `So`/`Si`/`Sd`/`Ss`, which per the Itanium ABI already denote the *complete
  specialization* `basic_ostream<char,std::char_traits<char>>` etc. — emit the abbreviation and
  STOP. Result today: `itanium_mangle_operator_sub("std::basic_ostream<char,std::char_traits<char>>",
  "<<", {"double"}, false)` returns `_ZNSoIcSt11char_traitsIcEElsEd` instead of `_ZNSolsEd`
  (confirmed by `tmp/mangle_probe.cpp`).
- `Ss` applies ONLY to the pre-C++11 `std::basic_string<char,char_traits<char>,allocator<char>>`,
  NOT `std::__cxx11::basic_string` — so the C++11 string symbols (which the existing tests cover)
  are unaffected. The existing `std_abbrev` already excludes `Ss` (comment `:506`); the new
  complete-abbrev helper adds it for the exact pre-cxx11 spelling only.
- Helpers available in the `ItaniumMangler` class: `canon_type(const TypeNode&)` (`:489`, a stable
  key string — builtin `char`→`"#c"`, `std::char_traits<char>`→`"std::char_traits<#c>"`,
  `std::allocator<char>`→`"std::allocator<#c>"`) and `scoped_prefix_noargs(chain,i)` (`:587`,
  e.g. `"std::basic_ostream"`). Both are `static`.
- The candidate-table comment (`:206-208`) confirms abbreviations consume NO numbered slot.

## File Structure

| File | Change |
|------|--------|
| `tests/unit/test_mangle.cpp` | **Add** a `TEST_SUITE` for the `So`/`Si`/`Sd` complete-spec abbreviations (member `operator<<`/`>>`, standalone `So`, ref `RSo`) + a regression check that `Sa`/`Sb` still expand. |
| `src/madc_mangle.cpp` | **Add** `ItaniumMangler::std_complete_abbrev(chain,i)`; **guard** the `encode_name` template branch to emit it standalone. |

---

## Task 1: Failing tests for the complete-specialization abbreviations

**Files:**
- Modify: `tests/unit/test_mangle.cpp` (append a new `TEST_SUITE` after the stringstream suite, before the "type encoder sanity" suite)

- [ ] **Step 1: Confirm the expected symbols against the real toolchain (Rule #1)**

Run (each must print the demangled signature, confirming the literal is the real libstdc++ symbol):
```bash
for s in _ZNSolsEd _ZNSolsEi _ZNSolsEl _ZNSolsEj _ZNSolsEPKv _ZNSirsERi; do printf '%s\t' "$s"; c++filt "$s"; done
```
Expected:
```
_ZNSolsEd	std::basic_ostream<char, std::char_traits<char> >::operator<<(double)
_ZNSolsEi	std::basic_ostream<char, std::char_traits<char> >::operator<<(int)
_ZNSolsEl	std::basic_ostream<char, std::char_traits<char> >::operator<<(long)
_ZNSolsEj	std::basic_ostream<char, std::char_traits<char> >::operator<<(unsigned int)
_ZNSolsEPKv	std::basic_ostream<char, std::char_traits<char> >::operator<<(void const*)
_ZNSirsERi	std::basic_istream<char, std::char_traits<char> >::operator>>(int&)
```

- [ ] **Step 2: Add the failing test suite**

Append to `tests/unit/test_mangle.cpp` (after the `std::stringstream` suite, ~line 276):
```cpp
TEST_SUITE("Itanium substitution: complete-spec abbreviations (So/Si/Sd)") {

	static const std::string OS =
		"std::basic_ostream<char,std::char_traits<char>>";
	static const std::string IS =
		"std::basic_istream<char,std::char_traits<char>>";

	TEST_CASE("ostream member operator<< overloads") {
		CHECK(itanium_mangle_operator_sub(OS, "<<", {"double"}, false)       == "_ZNSolsEd");
		CHECK(itanium_mangle_operator_sub(OS, "<<", {"int"}, false)          == "_ZNSolsEi");
		CHECK(itanium_mangle_operator_sub(OS, "<<", {"long"}, false)         == "_ZNSolsEl");
		CHECK(itanium_mangle_operator_sub(OS, "<<", {"unsigned int"}, false) == "_ZNSolsEj");
		CHECK(itanium_mangle_operator_sub(OS, "<<", {"const void*"}, false)  == "_ZNSolsEPKv");
	}

	TEST_CASE("istream member operator>>") {
		CHECK(itanium_mangle_operator_sub(IS, ">>", {"int&"}, false) == "_ZNSirsERi");
	}

	TEST_CASE("complete-spec abbreviation as a standalone type / reference") {
		CHECK(itanium_encode_type_sub(OS)        == "So");
		CHECK(itanium_encode_type_sub(OS + "&")  == "RSo");
		CHECK(itanium_encode_type_sub(IS)        == "Si");
	}

	TEST_CASE("regression: Sa/Sb still take template args (NOT complete)") {
		CHECK(itanium_encode_type_sub("std::allocator<char>") == "SaIcE");
		// __cxx11 string still spelled out (uses Sb-family, not Ss)
		CHECK(itanium_encode_type_sub(std_string_type())
		      == "NSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
	}
}
```

## Task 2: Run the tests — verify the new ones fail, existing pass

- [ ] **Step 1: Build the mangler object + compile the test standalone (fast iteration)**

Run:
```bash
make -C src obj/madc_mangle.o
( ulimit -t 60; g++ -std=c++11 -Iinclude tests/unit/test_mangle.cpp obj/madc_mangle.o -o tmp/test_mangle )
( ulimit -t 60; timeout 60 ./tmp/test_mangle )
```
Expected: the new `So/Si/Sd` suite **FAILS** — e.g. `_ZNSoIcSt11char_traitsIcEElsEd` (got) vs
`_ZNSolsEd` (expected); the standalone case gives `SoIcSt11char_traitsIcEE` vs `So`. **All
pre-existing suites (string/vector/map/set/stringstream/Sa) still PASS** — this proves the fix is
scoped to the complete-spec abbreviations and does not disturb `Sa`/`Sb`/`__cxx11`.

## Task 3: Fix the mangler — complete-spec abbreviations emit standalone

**Files:**
- Modify: `src/madc_mangle.cpp` (add `std_complete_abbrev` near `std_abbrev` ~`:511`; guard the `encode_name` template branch ~`:542`)

- [ ] **Step 1: Add the `std_complete_abbrev` helper**

In `class ItaniumMangler`, immediately after the `std_abbrev` function (after `:511`), add:
```cpp
	// Complete-specialization std abbreviations (Ss/Si/So/Sd): the abbreviation
	// stands for the ENTIRE specialization (template name + its canonical
	// char/char_traits[/allocator] args), so it is emitted standalone with NO
	// I..E expansion and consumes no slot. (St/Sa/Sb are prefix/template
	// abbreviations that DO take further args — handled by std_abbrev.)
	// Ss is the pre-C++11 std::basic_string ONLY (NOT std::__cxx11::basic_string).
	static std::string std_complete_abbrev(const std::vector<NameComponent> &chain,
	                                        size_t i)
	{
		const NameComponent &nc = chain[i];
		if (!nc.is_template) return "";
		std::string scope = scoped_prefix_noargs(chain, i);
		const std::vector<TypeNode> &a = nc.targs;
		bool ct = a.size() >= 2
		          && canon_type(a[0]) == "#c"
		          && canon_type(a[1]) == "std::char_traits<#c>";
		if (scope == "std::basic_istream"  && a.size() == 2 && ct) return "Si";
		if (scope == "std::basic_ostream"  && a.size() == 2 && ct) return "So";
		if (scope == "std::basic_iostream" && a.size() == 2 && ct) return "Sd";
		if (scope == "std::basic_string"   && a.size() == 3 && ct
		    && canon_type(a[2]) == "std::allocator<#c>")           return "Ss";
		return "";
	}
```

- [ ] **Step 2: Guard the template branch in `encode_name`**

In `encode_name`, the template branch begins with `} else {` (~`:542`). Insert at the very top of
that `else` block, before the existing `// Template-prefix` logic:
```cpp
			// Complete-specialization std abbreviation (Ss/Si/So/Sd): emit the
			// abbreviation standalone — it already includes the template args,
			// so do NOT expand I..E and do NOT add a slot.
			std::string whole = std_complete_abbrev(chain, i);
			if (!whole.empty()) {
				encoded = whole;
				continue;
			}
```
(Leave the rest of the `else` block — `pref`/`pabbr`/`tprefix`/`args`/`tidKey` — unchanged; it
still handles `Sa`/`Sb`/user templates.)

- [ ] **Step 3: Rebuild the object**

Run: `make -C src obj/madc_mangle.o`
Expected: compiles clean (no warnings).

## Task 4: Run the tests — verify all pass

- [ ] **Step 1: Recompile + run the standalone test**

Run:
```bash
( ulimit -t 60; g++ -std=c++11 -Iinclude tests/unit/test_mangle.cpp obj/madc_mangle.o -o tmp/test_mangle )
( ulimit -t 60; timeout 60 ./tmp/test_mangle )
```
Expected: **all suites PASS**, including the new `So/Si/Sd` suite (`_ZNSolsEd`, `_ZNSolsEi`,
`_ZNSolsEl`, `_ZNSolsEj`, `_ZNSolsEPKv`, `_ZNSirsERi`, standalone `So`/`RSo`/`Si`) AND the
unchanged string/vector/map/set/stringstream/`Sa` suites (regression proof).

## Task 5: Full unit-test gate + commit

- [ ] **Step 1: Run the full unit-test suite (capped per project rule)**

Run: `make -C src test 2>&1 | tail -15`
Expected: all doctest binaries pass (the mangler change is isolated; nothing else is touched).

- [ ] **Step 2: Build madc + integration safety net (the change is mangler-only; must be inert)**

Run: `make -C src 2>&1 | tail -3`
Then: `( ulimit -t 120; timeout 180 make -C src fulltest ) 2>&1 | tail -8`
Expected: integration count unchanged (still **457 pass / 6 fail / 55 skip** + flaky
`testfortypedcomma`) — proving this mangler fix changes no codegen behavior yet (the `_ZSt…`
literals still drive the live stream path; they are removed in a later plan).

- [ ] **Step 3: Commit**

```bash
git add src/madc_mangle.cpp tests/unit/test_mangle.cpp
git commit -m "fix(mangle): So/Si/Sd/Ss are complete-specialization abbreviations (no template args)

Per the Itanium ABI, So/Si/Sd/Ss already denote the full specialization
(basic_ostream<char,char_traits<char>> etc.), so they emit standalone with no
I..E expansion. The mangler was treating them like Sa/Sb (template prefixes) and
appending args -> _ZNSoIc...lsEd instead of _ZNSolsEd. This is the root-cause bug
that caused the stream operator symbols to be hardcoded as _ZSt literals.

No madc behavior change (the literals still drive codegen until a later plan
switches stream call-emission to the mangler). Verified vs c++filt / nm -D.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

- **Spec coverage:** implements spec work-item W1's first half (the `So`/`Si`/`Sd`/`Ss`
  complete-specialization fix) + extends the round-trip-vs-`c++filt` doctest. W1's second half
  (non-member std template operators, `_ZStls…` with `T_` params) and the codegen migration
  (delete the literals, route stream `<<` through the mangler) are SEPARATE follow-on plans —
  noted, not silently dropped.
- **Placeholder scan:** none — full code for the test suite, the helper, and the guard; exact
  commands with expected output.
- **Type consistency:** `std_complete_abbrev(const std::vector<NameComponent>&, size_t)` uses the
  existing `static` helpers `canon_type` and `scoped_prefix_noargs`; called from `encode_name` with
  `(chain, i)` in scope. Canonical keys (`"#c"`, `"std::char_traits<#c>"`, `"std::allocator<#c>"`)
  match `canon_type_core`/`canon_name_prefix` output.
- **Risk:** the only behavioral surface is symbol strings; the doctest pins them to real libstdc++
  symbols, and the full-suite gate proves no codegen regression. `Ss` is gated to the exact
  pre-cxx11 spelling so `__cxx11` string is untouched (existing tests prove it).
