# C++ Parser Correctness Track — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix three C++-correctness gaps surfaced while building RTTI (S5), so idiomatic C++ (const members, const methods, address-of-reference, virtual destructors) parses and lowers faithfully.

**Architecture:** Three independent fixes. Stage A (const) + Stage B (reference address-of) are small, localized parser/cir_builder changes — do them first. Stage C (virtual destructors) is genuinely a feature (vtable dtor slot + virtual dispatch on `delete`); it is scoped here as design-first and should get its own brainstorm/plan before execution.

**Tech Stack:** madc parser (`src/parser.cpp`), cir_builder (`src/cir_builder.cpp`), `FuncDef`/`DataDefCLASS` (`include/datadef.h`); doctest unit + `.mad` integration matched to g++; c2mir→MIR backend.

**Branch:** `feature/cpp-parser-correctness-claude`, cut from `feature/retire-std-hardcoding-claude` HEAD (so Stage C can be verified against the RTTI/typeid work). Merges back into the campaign branch when green.

---

## Context — root causes (pinned during S5)

These were diagnosed against g++/clang while building S5 RTTI. Each cites the exact site.

- **Gap #2a — `const` class member doesn't parse.** `const char *m;` in a class body → "Expecting type in class definition". The member/method loop (`src/parser.cpp:12067`) calls `resolve_declared_type_token` on the first token; `const` is a keyword, not a type, so it returns NULL and throws. Fix: consume an optional leading `const`/`volatile` qualifier before resolving the member type. (madc does not enforce const-correctness; consuming is faithful for a data member.)
- **Gap #2b — trailing-`const` method doesn't parse.** `int f() const { … }` → "Expecting brace after function declaration" (`src/parser.cpp:15199`): after the parameter list the parser demands `{`, but the next token is `const`. Fix: consume the trailing `const` and **record it** — `FuncDef::is_const_method` ALREADY exists and is consumed by the keystone mangler (`src/parser.cpp:11774`/`11777`, `itanium_mangle_{operator,member}_sub(..., fd->is_const_method)`), so a const method then mangles to `_ZNK…` correctly. Set `func->is_const_method = true` (the FuncDef is `func` at `src/parser.cpp:15173`).
- **Gap #3 — `&reference-param` yields address-of-the-pointer.** madc lowers `T&` params to `T*` (`vfREFERENCE`). `TokenAddrOf` lowering (`src/cir_builder.cpp:5186`) emits `node1(N_ADDR, id(var))` unconditionally — for a reference var that is `&(the pointer slot)` (a `T**`), not the referent's address. Fix: when `ta->var.flags & vfREFERENCE`, return `id(var)` directly (the pointer value already IS the referent's address). This is why `<typeinfo>`'s `operator==` had to compare `__name` instead of `this == &o`.
- **Gap #1 — virtual-destructor-only class isn't polymorphic.** `class W { virtual ~W(); };` does not set `has_vtable`. `is_virtual` is parsed (`src/parser.cpp:11993`) but the **dtor branch (`12004`) ignores it**, while the virtual-*method* path (`12209`) sets `virtual_methods`/`vtable_slots`/`has_vtable`. Consequence: `typeid`/`dynamic_cast`/virtual dispatch on a dtor-only polymorphic base all take wrong paths; `_ZTI` is referenced but not emitted. **This is bigger than #2/#3 — see Stage C.**

---

# Stage A — const members and const methods

### Task A1: parse a leading `const` on a class member

**Files:**
- Modify: `src/parser.cpp` (member/method loop, ~12065)
- Test: `tests/test_cpp_const_member.mad` (+ `.expect`)

- [ ] **Step 1: Write the failing integration test.** Create `tests/test_cpp_const_member.mad`:

```cpp
#include <iostream>
using namespace std;
class Box {
    const char *label;   // const data member
public:
    void set(const char *s) { label = s; }
    const char *get() { return label; }
};
int main() {
    Box b;
    b.set("hi");
    cout << b.get() << endl;   // hi
    return 0;
}
```
Create `tests/test_cpp_const_member.expect`:
```
hi
```
Verify the oracle: `g++ -std=c++17 -x c++ tests/test_cpp_const_member.mad -o tmp/cm && tmp/cm` → `hi`. (NB: this also uses a `const char*` PARAMETER on a method — confirm madc already accepts that; if `void set(const char *s)` also fails to parse, the same leading-const fix in the parameter parser is needed — check `parseFunction`'s parameter loop and apply the same consume there. The S5 `dynamic_cast`/`__dynamic_cast` work showed `const`-param functions already parse via `need_output_extern`, so parameters likely already work; confirm in Step 2.)

- [ ] **Step 2: Run it, verify it fails.**
Run: `( ulimit -t 60; ./bin/madc tests/test_cpp_const_member.mad 2>&1 | grep -v setrlimit | tail -3 )`
Expected: "Expecting type in class definition" at the `const char *label;` line. (If the failure is instead on the `const char *s` parameter, note it and extend Step 3 to the parameter parser.)

- [ ] **Step 3: Implement.** In `src/parser.cpp`, just before the member-type resolution at line ~12065 (after the virtual/dtor/ctor branches, before `TokenDataType *mtype = resolve_declared_type_token(...)`), consume optional CV-qualifiers:

```cpp
	// Optional leading cv-qualifiers on a data member / method return type
	// (`const char *m;`, `volatile int v;`). madc does not enforce member
	// const-correctness, so consume them. (A const data member is treated as
	// an ordinary member.)
	while ( pgm.peekToken()
	     && pgm.peekToken()->type() == TokenType::ttIdentifier
	     && ( ((TokenIdent *)pgm.peekToken())->str == "const"
	       || ((TokenIdent *)pgm.peekToken())->str == "volatile" ) )
	    pgm.nextToken();
```
(Use the same `ttIdentifier`+string test the `virtual` branch at 11994 uses, since `const`/`volatile` lex as identifiers/keywords the same way. If they lex as a dedicated token id — check with `grep -n '"const"' src/lexer.cpp` — match on that id instead; confirm before writing.)

- [ ] **Step 4: Run it, verify it passes.**
Run: `( ulimit -t 60; ./bin/madc tests/test_cpp_const_member.mad 2>&1 | grep -v setrlimit )`
Expected: `hi`

- [ ] **Step 5: Regression gate.** Build 0-warn; `bash scripts/run_tests.sh` shows only the known-6 + flaky `testfortypedcomma`; SMAUG boots; no-std gate unchanged (468).

- [ ] **Step 6: Commit.**
```bash
git add src/parser.cpp tests/test_cpp_const_member.mad tests/test_cpp_const_member.expect
git commit -m "feat(parser): parse leading const/volatile on class members (gap 2a)"
```

### Task A2: parse + record a trailing `const` method qualifier

**Files:**
- Modify: `src/parser.cpp` (method declarator, ~15196 — just before the brace check at 15199, AND before the declaration-only check at 15168 so a `T f() const;` prototype also parses)
- Test: `tests/test_cpp_const_method.mad` (+ `.expect`)

- [ ] **Step 1: Find where the parameter list close-paren is consumed.** The trailing `const` must be eaten AFTER `)` and BEFORE both the declaration-only check (`src/parser.cpp:15168`, which inspects the next token for `;` vs `{`) and the brace check (`15199`). Read `src/parser.cpp:15120-15205` (`grep -n "Expecting brace after function declaration" src/parser.cpp` → 15199) and locate the `nt = nextToken()` / peek right after the parameter `)` is consumed. That is the insertion point.

- [ ] **Step 2: Write the failing test.** Create `tests/test_cpp_const_method.mad`:

```cpp
#include <iostream>
using namespace std;
class Counter {
    int n;
public:
    Counter() { n = 7; }
    int get() const { return n; }       // const method
};
int main() {
    Counter c;
    cout << c.get() << endl;            // 7
    return 0;
}
```
Create `tests/test_cpp_const_method.expect`:
```
7
```
Oracle: `g++ -std=c++17 -x c++ tests/test_cpp_const_method.mad -o tmp/cmeth && tmp/cmeth` → `7`.

- [ ] **Step 3: Run it, verify it fails.**
Run: `( ulimit -t 60; ./bin/madc tests/test_cpp_const_method.mad 2>&1 | grep -v setrlimit | tail -3 )`
Expected: "Expecting brace after function declaration" at the `int get() const` line.

- [ ] **Step 4: Implement.** At the insertion point found in Step 1 (right after the parameter-list `)` is consumed, before the declaration-only/brace decision), consume the trailing qualifier and record it on the FuncDef (`func` is in scope — see `src/parser.cpp:15173`):

```cpp
	// Trailing method cv/ref/exception/override qualifiers: `T m() const`,
	// `... noexcept`, `... override`, `... final`. Record const (drives Itanium
	// mangling: a const method is _ZNK… — FuncDef::is_const_method is consumed
	// by the keystone, src/parser.cpp:11774/11777). The others are consumed
	// (madc does not yet model them).
	for (;;) {
	    TokenBase *q = peekToken();
	    if ( !q || q->type() != TokenType::ttIdentifier ) break;
	    const std::string &qs = ((TokenIdent *)q)->str;
	    if ( qs == "const" )         { func->is_const_method = true; nextToken(); }
	    else if ( qs == "volatile" || qs == "noexcept"
	           || qs == "override"  || qs == "final" )   { nextToken(); }
	    else break;
	}
```
(Confirm the exact accessor name for the FuncDef at this site — `func` per 15173 — and that `is_const_method` is a public bool on `FuncDef` via `grep -n "is_const_method" include/datadef.h`.)

- [ ] **Step 5: Run it, verify it passes.**
Run: `( ulimit -t 60; ./bin/madc tests/test_cpp_const_method.mad 2>&1 | grep -v setrlimit )`
Expected: `7`

- [ ] **Step 6: Verify const-method mangling.** Add a polymorphic-free check that a const method on a std::-bound class mangles to `_ZNK…` — OR, simplest, confirm the existing string `c_str()`/`length()` keystone (already passing `true` for const at `src/parser.cpp:11774`) is unaffected. Re-run `./bin/test_mangle_rtti` and the string tests; both green.

- [ ] **Step 7: Regression gate** (build 0-warn; run_tests known-6 + flaky; SMAUG boots; no-std gate 468).

- [ ] **Step 8: Commit.**
```bash
git add src/parser.cpp tests/test_cpp_const_method.mad tests/test_cpp_const_method.expect
git commit -m "feat(parser): parse + record trailing const method qualifier (gap 2b)"
```

---

# Stage B — address-of a reference

### Task B1: `&ref` yields the referent address, not address-of-the-pointer

**Files:**
- Modify: `src/cir_builder.cpp` (`TokenAddrOf` lowering, ~5181-5187)
- Test: `tests/test_cpp_addr_ref.mad` (+ `.expect`)

- [ ] **Step 1: Write the failing test.** A function takes a reference and returns the address of the referent; the caller compares it to the address of the original object (must be equal). Create `tests/test_cpp_addr_ref.mad`:

```cpp
#include <iostream>
using namespace std;
long addr_of(int &r) { return (long)&r; }   // &r must be the referent's address
int main() {
    int x = 5;
    cout << (addr_of(x) == (long)&x) << endl;   // 1
    return 0;
}
```
Create `tests/test_cpp_addr_ref.expect`:
```
1
```
Oracle: `g++ -std=c++17 -x c++ tests/test_cpp_addr_ref.mad -o tmp/ar && tmp/ar` → `1`.

- [ ] **Step 2: Run it, verify it fails.**
Run: `( ulimit -t 60; ./bin/madc tests/test_cpp_addr_ref.mad 2>&1 | grep -v setrlimit )`
Expected: `0` (madc currently returns `&(pointer slot)`, which is not `&x`), or a type warning about `int**`.

- [ ] **Step 3: Implement.** In `src/cir_builder.cpp`, the `TokenAddrOf` block (~5181):

```cpp
		TokenAddrOf *ta = dynamic_cast<TokenAddrOf *>(tb);
		if (ta) {
			// `&capturedvar` inside a nested fn IS the capture pointer param.
			if (note_capture(&ta->var))
				return id(ta->var.name.c_str(), tb);
			// `&ref` where ref is a T& parameter (lowered to a T* with
			// vfREFERENCE): the variable's VALUE is already the referent's
			// address, so &ref is just that value — NOT N_ADDR(slot) (which
			// would be a T**). (gap 3)
			if (ta->var.flags & vfREFERENCE)
				return id(var_emit_name(ta->var).c_str(), tb);
			return node1(N_ADDR, id(var_emit_name(ta->var).c_str(), tb), tb);
		}
```

- [ ] **Step 4: Run it, verify it passes.**
Run: `( ulimit -t 60; ./bin/madc tests/test_cpp_addr_ref.mad 2>&1 | grep -v setrlimit )`
Expected: `1`

- [ ] **Step 5: Regression gate** (build 0-warn; run_tests known-6 + flaky; SMAUG boots — SMAUG uses reference params heavily, so this is the key regression check; no-std gate 468).

- [ ] **Step 6: Commit.**
```bash
git add src/cir_builder.cpp tests/test_cpp_addr_ref.mad tests/test_cpp_addr_ref.expect
git commit -m "feat(cir): &reference yields the referent address, not address-of-pointer (gap 3)"
```

---

# Stage C — virtual destructors (DESIGN-FIRST — own brainstorm/plan before executing)

**This is not a one-line gap; it is a feature.** Making `class W { virtual ~W(); };` polymorphic requires more than flipping `has_vtable`:

1. **Parser:** in the dtor branch (`src/parser.cpp:12004`), when `is_virtual` (parsed at `11993`), register the destructor as a virtual slot: `ddc->virtual_methods["~"+tag->str] = true;`, push a dtor slot into `ddc->vtable_slots`, and set `ddc->has_vtable = true` (mirroring the virtual-method path at `12209`). A derived class overriding the dtor must resolve to the most-derived dtor in that slot.
2. **Vtable emission (`src/cir_builder.cpp` `class_vtable_def`):** the dtor slot must hold the class's complete-object destructor symbol (`class_complete_dtor_symbol`, from MI S4). Itanium actually places TWO dtor entries (D1 complete, D0 deleting) per polymorphic class; decide whether madc emits one (its own `_dtor_complete`) or both. NB `class_vtable_def` currently returns NULL when `vtable_slots` is empty — a dtor-only class must still emit a vtable (with the S5a prologue) so `_ZTI` is reachable.
3. **Virtual dispatch on `delete`:** `delete base_ptr` where the static type has a virtual dtor must dispatch through the vtable to the most-derived complete dtor (today `delete` calls the static-type dtor by name). This is the real behavioral change — find the `delete` lowering (`grep -n "TokenDELETE\|class_complete_dtor_symbol\|del_class" src/cir_builder.cpp`) and route it through the dtor vtable slot when the class is polymorphic.
4. **Interaction with MI/S4:** the complete-object dtor split (S4) and the grouped-vtable dtor slot must agree on the symbol and `this`-adjustment for secondary bases.

**Recommendation:** treat Stage C as its own `superpowers:brainstorming` → `writing-plans` cycle (spec the D1/D0 question, the dtor-only-vtable emission, and virtual-`delete` dispatch against g++ `-fdump-vtable-layouts`) rather than executing it inline from this plan. It depends on and extends the MI vtable model (S3/S4), so it belongs near that work. Until it lands, a virtual-method (non-dtor) makes a class polymorphic, which is the workaround used in `tests/test_rtti_name.mad`.

**Minimal-correct interim (if a quick unblock is wanted, NOT a substitute for the feature):** parser step 1 above + relax `class_vtable_def` to emit a prologue-only vtable when `has_vptr_slot` and `vtable_slots` is empty, so `typeid`/`dynamic_cast` work on a dtor-only base. This does NOT give virtual `delete` dispatch — only RTTI. Land it ONLY with a comment + test documenting that `delete base*` does not yet run the derived dtor, so it is not mistaken for the full feature.

---

## Self-Review

**1. Spec coverage:** Gap #2a → Task A1. Gap #2b (parse + record for mangling) → Task A2 (sets the existing `FuncDef::is_const_method`). Gap #3 → Task B1. Gap #1 → Stage C (scoped design-first, with the precise anchors + the D1/D0 / dtor-only-vtable / virtual-delete subtleties called out so it isn't half-fixed).

**2. Placeholder scan:** A1/A2/B1 contain complete code and exact commands. The two "confirm X" notes (does `const` lex as a keyword id vs identifier; exact FuncDef accessor at 15173) are explicit verification steps with the grep to run, not vague TODOs — they hedge a fact the executor must check before writing, because line numbers/lexing can drift. Stage C is deliberately design-first (the scope-check in writing-plans permits splitting an independent subsystem into its own plan) and says so plainly rather than faking detailed tasks for dispatch work that needs its own recon.

**3. Type consistency:** `FuncDef::is_const_method` (A2) is the existing field consumed at `src/parser.cpp:11774/11777`. `vfREFERENCE` (B1) is the existing flag (`include/datadef.h:79`). `var_emit_name`, `note_capture`, `id`, `node1`, `N_ADDR` (B1) are the existing cir_builder helpers used in the surrounding `TokenAddrOf` block. `is_virtual`/`virtual_methods`/`vtable_slots`/`has_vtable`/`class_complete_dtor_symbol` (Stage C) are the existing parser/MI names.
