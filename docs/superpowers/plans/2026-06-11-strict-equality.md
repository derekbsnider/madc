# `===` / `!==` Strict Equality Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the STD_MADC strict-equality operators `===` / `!==` (type-domain identity AND value equality) for all operand kinds, including user `operator===`/`operator!==` overloading, per the approved spec `docs/superpowers/specs/2026-06-11-strict-equality-design.md`.

**Architecture:** Lexer gates the tokens to STD_MADC (like `<=>`); the parser already handles them generically (`Token3Eq` is a `TokenMultiOp`, and `parseOperatorId` accepts any `TokenMultiOp` after `operator`); CirBuilder lowers `tk3Eq`/`tk3NotEq` at Tier 1 — scalars via a new `DataDef::same_representation()` predicate, class operands via the existing `class_operator_call` dispatch (user `operator===` first, then `operator==` as the domain rule), statically-false via `(l, r, 0|1)` comma nodes. Zero MIR-fork changes.

**Tech Stack:** C++11, tabs; `make -C src`; doctest units; `tests/*.mad` integration tests with `.expect`/`.expect_err`/`.flags` fixtures.

**Standing project rules that bind every task:** single shell commands (no `&&` chains); cap heavy runs `( ulimit -t 600; timeout 900 <cmd> )`; ONE heavy job at a time; commit after each green task; `make -C src` does NOT relink `bin/test_*` — run `make -C src test` before trusting unit results.

**Known facts the implementer must NOT re-derive** (verified 2026-06-11 at develop+spec HEAD):
- `Token3Eq` exists (`include/tokens.h:696`, id `tk3Eq`, precedence 7); the lexer emits it **unconditionally** at `src/lexer.cpp:2055`; `src/pch.cpp:150` reconstructs it. There is no `!==` token.
- CIR has **no** tk3Eq lowering: `a === b` today dies with "unhandled binary operator" from the default case of the binary switch (`src/cir_builder.cpp` ~7860).
- The `<=>` gating precedent is `src/lexer.cpp:2900-2907` (`language_std == STD_MADC || language_std >= STD_CPP20`); the gate test precedent is `tests/test3waygate.{mad,flags,expect_err}` (flags `--std=c++17 --no-embedded-headers`, expect_err `Missing operand`).
- `binop_overload_symbol` (`src/cir_builder.cpp:4805`) maps TokenID → operator spelling for `class_operator_call` (`:6108`), which is invoked for every binary op at `:7765` BEFORE the scalar switch. Free operators with bodies at global scope dispatch (testfreeop.mad); `select_operator_overload` does ctor-conversion scoring.
- `DataType` tags ARE a representation encoding: `dtCHAR == dtINT8`, long and long long are both `dtINT64`, `dtBOOL`/`dtFLOAT`/`dtDOUBLE`/`dtLDOUBLE` distinct; pointers = base+10000, references = base+20000 (`include/datadef.h:37-64`). `DataDefPTR` carries `base_type`; `DataDefREF` is-a `DataDefPTR` with `is_reference()`; `DataDefENUM` (`datadef.h:916`) carries `enum_name` but tags as `dtINT`; `DataDefCArray` tags as `dtRESERVED`; `DataDefFPTR` (`datadef.h:1039`) wraps `FuncDef *target`; `FuncDef` (`include/madc.h:48`) has `DataDef &returns` + `std::vector<DataDef *> parameters`.
- `madc::value::operator==` is already tag-strict (`src/madc_value.cpp:253`). BUT script-side `array` is the builtin `dtARRAY` runtime-object type (`parser.cpp:8645`, `cir_builder.cpp:885-895`), NOT a DataDefCLASS, and `value`'s int/double/bool ctors are `explicit` — Task 7 investigates that surface separately.
- The mangler's single operator table is `operator_code` (`src/madc_mangle.cpp:147`); `itanium_mangle_operator` and the member path both consume it.
- The eval-DSL rewrites `===` on string operands (`src/madc_program.cpp:395-535`); its validator blocklists operators (`is_allowed_expression_operator_id`, `:1145`) so new ids are allowed by default.

---

### Task 0: Feature branch

- [ ] **Step 0.1:** `git -C /workspace/madc checkout -b feature/strict-equality-claude`
- [ ] **Step 0.2:** `git -C /workspace/madc log --oneline -2` — confirm the spec commit `ba7cd96` is in history and the working tree is clean.

---

### Task 1: `Token3NotEq` + STD_MADC lexer gating + gate tests

**Files:**
- Modify: `include/tokens.h` (TokenID enum tail ~line 54; new class after `Token3Eq` ~line 706)
- Modify: `src/lexer.cpp:2051-2059` (`'='` case), `src/lexer.cpp:2715-2716` (`'!'` case)
- Modify: `src/pch.cpp:150` (clone case)
- Create: `tests/test3eqgate.mad`, `tests/test3eqgate.flags`, `tests/test3eqgate.expect_err`
- Create: `tests/test3noteqgate.mad`, `tests/test3noteqgate.flags`, `tests/test3noteqgate.expect_err`

- [ ] **Step 1.1: Write the failing gate tests.**

`tests/test3eqgate.mad`:
```c
// === is a madc-dialect (STD_MADC) token only. Below the std floor the
// sequence lexes as `==` then `=`, which cannot parse in an expression —
// exactly how a conforming C++ compiler rejects it. See .expect_err.
int main()
{
	int a = 5, b = 5;
	int r = (a === b);
	return r;
}
```
`tests/test3eqgate.flags`:
```
--std=c++17 --no-embedded-headers
```
`tests/test3noteqgate.mad`:
```c
// !== is a madc-dialect (STD_MADC) token only. Below the std floor the
// sequence lexes as `!=` then `=` — a conforming syntax error. See .expect_err.
int main()
{
	int a = 5, b = 6;
	int r = (a !== b);
	return r;
}
```
`tests/test3noteqgate.flags`:
```
--std=c++17 --no-embedded-headers
```

- [ ] **Step 1.2: Capture the real conforming error text** (don't guess). Run:
`bin/madc --std=c++17 --no-embedded-headers tests/test3noteqgate.mad` (this already lexes `!=` `=` today, so it shows the post-gate error shape `===` will produce too).
Put the stable first error line (expected: `Missing operand`, the test3waygate precedent) into BOTH `tests/test3eqgate.expect_err` and `tests/test3noteqgate.expect_err`. One line each.

- [ ] **Step 1.3: Verify test3eqgate FAILS now.** Run: `bin/madc --std=c++17 --no-embedded-headers tests/test3eqgate.mad` then `echo $?`.
Expected today: the `===` token lexes (ungated bug) and CIR reports `unhandled binary operator` — which does NOT contain the `.expect_err` line, so the fixture fails. This is the failing-test baseline.

- [ ] **Step 1.4: Add `tk3NotEq` to the TokenID enum.** In `include/tokens.h`, the enum's tail currently reads (~line 48-55):
```cpp
  tkUNION, tkNEW, tkDELETE,
  tkDynamicCast, tkTypeid,    // RTTI (S5): dynamic_cast<T*>(e), typeid(e|T)
  tkObjTemp                   // functional construction temporary: T(args)
};
```
Append at the END (PCH serializes numeric ids — never renumber the middle):
```cpp
  tkUNION, tkNEW, tkDELETE,
  tkDynamicCast, tkTypeid,    // RTTI (S5): dynamic_cast<T*>(e), typeid(e|T)
  tkObjTemp,                  // functional construction temporary: T(args)
  tk3NotEq                    // !== strict not-equal (STD_MADC dialect)
};
```

- [ ] **Step 1.5: Add the `Token3NotEq` class** in `include/tokens.h` immediately after `Token3Eq` (after line ~706). The fold predicates mirror `Token3Eq` (type-tag equality IS representation identity for the simple literal types folding can see — keep them consistent, inverted):
```cpp
// comparison operator !== (not exactly equal to) — !(===)
class Token3NotEq: public TokenMultiOp
{
public:
    Token3NotEq() : TokenMultiOp("!==") {}
    virtual TokenID id() const { return TokenID::tk3NotEq; }
    virtual TokenBase *clone() { return new Token3NotEq(); }
    virtual inline int precedence() const { return 7; }
    inline int64_t ioperate() const { return (left->datatype() == right->datatype() && left->ival() == right->ival()) ? 0 : 1; }
    inline double foperate() const { return (left->datatype() == right->datatype() && left->dval() == right->dval()) ? 0 : 1; }
};
```

- [ ] **Step 1.6: Gate the lexer.** `src/lexer.cpp:2051-2059`, replace the `'='` case body:
```cpp
	case '=':
	    if (source.peek() == '=')
	    {
		source.get();
		// === is the madc dialect's strict-equality token. Below the
		// std floor the sequence lexes as == then = — C/C++ sources
		// keep their conforming syntax error.
		if (source.peek() == '=' && language_std == STD_MADC)
		    { source.get(); return new Token3Eq; }		// ===
		return new TokenEquals;					// ==
	    }
	    if (source.peek() == '>') { source.get(); return new TokenFatArrow; } // =>
	    return new TokenAssign;					// =
```
And `src/lexer.cpp:2715-2716`, replace the `'!'` case:
```cpp
	case '!': if (source.peek() != '=') return new TokenLnot;		// !
	    source.get();
	    // !== is the madc dialect's strict not-equal; below the floor it
	    // lexes as != then = (conforming syntax error).
	    if (source.peek() == '=' && language_std == STD_MADC)
		{ source.get(); return new Token3NotEq; }		// !==
	    return new TokenNotEq;					// !=
```

- [ ] **Step 1.7: PCH reconstruction.** In `src/pch.cpp` next to line 150 add:
```cpp
    case TokenID::tk3NotEq: return new Token3NotEq();
```

- [ ] **Step 1.8: Build.** Run: `( ulimit -t 600; timeout 900 make -C src )` — must compile warning-free.

- [ ] **Step 1.9: Verify both gate tests pass.** Run each: `bin/madc --std=c++17 --no-embedded-headers tests/test3eqgate.mad` (and `test3noteqgate.mad`); each must exit nonzero with stderr containing the `.expect_err` line. Also sanity-check STD_MADC still lexes the token: `bin/madc tests/test3eqgate.mad` (no `--std`) should now fail with `unhandled binary operator` — proving `===` lexes in the dialect (lowering comes in Task 3).

- [ ] **Step 1.10: Commit.**
```bash
git add include/tokens.h src/lexer.cpp src/pch.cpp tests/test3eqgate.mad tests/test3eqgate.flags tests/test3eqgate.expect_err tests/test3noteqgate.mad tests/test3noteqgate.flags tests/test3noteqgate.expect_err
git commit -m "feat(lexer): !== token; gate ===/!== to STD_MADC (conformance fix)"
```

---

### Task 2: `DataDef::same_representation()` + doctest unit

**Files:**
- Modify: `include/datadef.h` (declaration inside `class DataDef`, next to `marshals_value_text()` ~line 115)
- Modify: `src/parser.cpp` (definition — put it near the other DataDef out-of-line methods, e.g. before `DataDefCLASS::binary_operator_return_type` at ~6335)
- Modify: `tests/unit/test_datadef.cpp` (new TEST_SUITE at end of file)

- [ ] **Step 2.1: Write the failing doctest.** Append to `tests/unit/test_datadef.cpp`:
```cpp
TEST_SUITE("DataDef::same_representation (=== type-domain identity)") {
    TEST_CASE("integers: size+signedness; tags are the representation") {
        DataDef i32("int", 4, DataType::dtINT32);
        DataDef u32("uint32_t", 4, DataType::dtUINT32);
        DataDef u8("uint8_t", 1, DataType::dtUINT8);
        DataDef i64a("long", 8, DataType::dtINT64);
        DataDef i64b("long long", 8, DataType::dtINT64);
        DataDef ch("char", 1, DataType::dtCHAR);
        DataDef i8("int8_t", 1, DataType::dtINT8);
        CHECK(!i32.same_representation(u32));   // signedness
        CHECK(!u32.same_representation(u8));    // size
        CHECK(i64a.same_representation(i64b));  // long === long long
        CHECK(ch.same_representation(i8));      // char == int8 on this target
        CHECK(!ch.same_representation(u8));
        CHECK(i32.same_representation(i32));
    }
    TEST_CASE("kinds: bool/float/int are distinct domains") {
        DataDef b("bool", 1, DataType::dtBOOL);
        DataDef u8("uint8_t", 1, DataType::dtUINT8);
        DataDef f("float", 4, DataType::dtFLOAT);
        DataDef d("double", 8, DataType::dtDOUBLE);
        DataDef i32("int", 4, DataType::dtINT32);
        CHECK(!b.same_representation(u8));
        CHECK(!f.same_representation(d));
        CHECK(!i32.same_representation(d));
        CHECK(d.same_representation(d));
    }
    TEST_CASE("enums are their own domain") {
        DataDefENUM color("Color");
        DataDefENUM color2("Color");
        DataDefENUM fruit("Fruit");
        DataDef i32("int", 4, DataType::dtINT32);
        CHECK(!color.same_representation(i32));   // enum vs int: false
        CHECK(!i32.same_representation(color));
        CHECK(!color.same_representation(fruit)); // different enums
        CHECK(color.same_representation(color2)); // same enum name
    }
    TEST_CASE("pointers recurse on the pointee; refs compare as referee") {
        DataDef i32("int", 4, DataType::dtINT32);
        DataDef u32("uint32_t", 4, DataType::dtUINT32);
        DataDefPTR pi(i32);
        DataDefPTR pi2(i32);
        DataDefPTR pu(u32);
        DataDefREF ri(i32);
        CHECK(pi.same_representation(pi2));
        CHECK(!pi.same_representation(pu));    // pointee signedness
        CHECK(!pi.same_representation(i32));   // ptr vs non-ptr
        CHECK(ri.same_representation(i32));    // T& reads as T
    }
    TEST_CASE("C arrays compare element + count") {
        DataDef i32("int", 4, DataType::dtINT32);
        DataDef u32("uint32_t", 4, DataType::dtUINT32);
        DataDefCArray a4(i32, "int[4]", 4);
        DataDefCArray b4(i32, "int[4]", 4);
        DataDefCArray a5(i32, "int[5]", 5);
        DataDefCArray u4(u32, "uint32_t[4]", 4);
        CHECK(a4.same_representation(b4));
        CHECK(!a4.same_representation(a5));
        CHECK(!a4.same_representation(u4));
    }
}
```

- [ ] **Step 2.2: Verify it fails to compile.** Run: `( ulimit -t 600; timeout 900 make -C src test )` — expected: compile error `no member named 'same_representation'`.

- [ ] **Step 2.3: Declare** in `include/datadef.h` inside `class DataDef`, after the `marshals_value_text()` declaration:
```cpp
    // Strict-equality (===) type-domain identity: do two types share one
    // value domain? Spec: docs/superpowers/specs/2026-06-11-strict-equality-design.md
    // §2.1. Defined in src/parser.cpp (needs the DataDef subclass set).
    bool same_representation(DataDef &d);
```

- [ ] **Step 2.4: Define** in `src/parser.cpp` (before `DataDefCLASS::binary_operator_return_type`, ~line 6335):
```cpp
// === type-domain identity (spec §2.1). Typedefs are already resolved to the
// underlying DataDef before this is called; const/volatile never reach DataDef.
// The DataType tag IS the representation for simple scalars (dtCHAR==dtINT8,
// long and long long are both dtINT64, bool/float/double/ldouble distinct).
bool DataDef::same_representation(DataDef &d)
{
    DataDef *a = this, *b = &d;
    // References compare as their referee (=== reads rvalues).
    while ( a->is_reference() )
	a = static_cast<DataDefPTR *>(a)->base_type;
    while ( b->is_reference() )
	b = static_cast<DataDefPTR *>(b)->base_type;
    if ( a == b )
	return true;
    // Enums are their own domain: only the same enum type matches.
    DataDefENUM *ae = dynamic_cast<DataDefENUM *>(a);
    DataDefENUM *be = dynamic_cast<DataDefENUM *>(b);
    if ( ae || be )
	return ae && be && ae->enum_name == be->enum_name;
    // Function (pointer) types: identical signature.
    if ( a->is_function() || b->is_function() )
    {
	if ( !a->is_function() || !b->is_function() )
	    return false;
	FuncDef *af = dynamic_cast<FuncDef *>(a);
	FuncDef *bf = dynamic_cast<FuncDef *>(b);
	if ( DataDefFPTR *afp = dynamic_cast<DataDefFPTR *>(a) )
	    af = afp->target;
	if ( DataDefFPTR *bfp = dynamic_cast<DataDefFPTR *>(b) )
	    bf = bfp->target;
	if ( !af || !bf )
	    return af == bf;
	if ( !af->returns.same_representation(bf->returns) )
	    return false;
	if ( af->parameters.size() != bf->parameters.size() )
	    return false;
	for ( size_t i = 0; i < af->parameters.size(); ++i )
	{
	    if ( !af->parameters[i] || !bf->parameters[i] )
		return af->parameters[i] == bf->parameters[i];
	    if ( !af->parameters[i]->same_representation(*bf->parameters[i]) )
		return false;
	}
	return true;
    }
    // C arrays: element domain + count.
    DataDefCArray *aa = dynamic_cast<DataDefCArray *>(a);
    DataDefCArray *ba = dynamic_cast<DataDefCArray *>(b);
    if ( aa || ba )
    {
	if ( !aa || !ba || aa->count != ba->count )
	    return false;
	return aa->element_type->same_representation(*ba->element_type);
    }
    // Class/struct/array-object/complex/simd types: identity — the a == b
    // fast path above; different instances are different domains.
    if ( a->basetype() != BaseType::btSimple || b->basetype() != BaseType::btSimple )
	return false;
    // Pointers: recurse on the pointee when both sides carry one. (The
    // builtin dtXXXptr tags encode a simple pointee, and DataDefPTR sets the
    // same tag, so the tag compare below also covers pointer-to-simple.)
    DataDefPTR *ap = dynamic_cast<DataDefPTR *>(a);
    DataDefPTR *bp = dynamic_cast<DataDefPTR *>(b);
    if ( ap && bp )
	return ap->base_type->same_representation(*bp->base_type);
    if ( a->is_pointer() != b->is_pointer() )
	return false;
    // Simple scalars (and same-tag builtin pointers): exact tag equality.
    return a->type() == b->type();
}
```

- [ ] **Step 2.5: Run the unit tests.** `( ulimit -t 600; timeout 900 make -C src test )` — all doctest cases pass, zero failures.

- [ ] **Step 2.6: Commit.**
```bash
git add include/datadef.h src/parser.cpp tests/unit/test_datadef.cpp
git commit -m "feat(types): DataDef::same_representation — === type-domain identity predicate"
```

---

### Task 3: CIR scalar lowering + `tests/test3eq.mad`

**Files:**
- Modify: `src/cir_builder.h:605` area (declarations)
- Modify: `src/cir_builder.cpp` (`binop_overload_symbol` ~4808; `class_operator_call` ~6108; new method near `three_way_builtin_lowering` ~6234; hook ~7821)
- Create: `tests/test3eq.mad`, `tests/test3eq.expect`

- [ ] **Step 3.1: Write the failing integration test.** `tests/test3eq.mad`:
```c
// === / !== strict equality: type-domain identity AND value equality.
// Spec: docs/superpowers/specs/2026-06-11-strict-equality-design.md
#include <cstdio>

int g_calls = 0;
int iside(int v) { g_calls++; return v; }
double dside(double v) { g_calls++; return v; }

enum Color { RED = 5 };
enum Fruit { APPLE = 5 };

int main()
{
	uint32_t a = 5;
	int32_t  b = 5;
	uint8_t  c = 5;

	printf("eq_ab=%d\n", a == b);		// loose: 1
	printf("eq_ac=%d\n", a == c);		// loose: 1
	printf("s_ab=%d\n", a === b);		// 0: signedness differs
	printf("s_ac=%d\n", a === c);		// 0: size differs
	printf("n_ab=%d\n", a !== b);		// 1

	uint32_t a2 = 5;
	printf("s_aa2=%d\n", a === a2);		// 1: same type same value
	printf("s_a6=%d\n", a === 6u);		// 0: same type diff value
	printf("n_aa2=%d\n", a !== a2);		// 0

	long lv = 7;
	long long llv = 7;
	printf("s_long=%d\n", lv === llv);	// 1: both 64-bit signed

	double d = 5;
	float f = 5;
	double d2 = 5;
	printf("s_df=%d\n", d === f);		// 0: float vs double
	printf("s_dd=%d\n", d === d2);		// 1

	bool t = true;
	uint8_t u1 = 1;
	printf("s_bu=%d\n", t === u1);		// 0: bool is its own kind

	char ch = 5;
	int8_t i8 = 5;
	printf("s_ci8=%d\n", ch === i8);	// 1: char==int8_t here
	printf("s_cu8=%d\n", ch === c);		// 0: char vs uint8_t

	printf("s_a5=%d\n", a === 5);		// 0: int literal vs uint32_t
	printf("s_a5u=%d\n", a === 5u);		// 1: suffix gives the type

	enum Color col = RED;
	enum Color col2 = RED;
	enum Fruit fr = APPLE;
	int five = 5;
	printf("s_ei=%d\n", col === five);	// 0: enum vs int
	printf("s_ee=%d\n", col === fr);	// 0: different enums
	printf("s_cc=%d\n", col === col2);	// 1: same enum same value

	int x = 1;
	int32_t y = 1;
	int *px = &x;
	int32_t *py = &y;
	uint32_t *pu = (uint32_t *)&x;
	printf("s_pxy=%d\n", px === py);	// 0: same domain, diff address
	printf("s_pxx=%d\n", px === &x);	// 1
	printf("s_pxu=%d\n", px === pu);	// 0: pointee signedness differs

	// A statically-false compare still evaluates both operands.
	int r = (iside(1) === dside(1.0));
	printf("sfx=%d calls=%d\n", r, g_calls);	// sfx=0 calls=2
	return 0;
}
```
`tests/test3eq.expect` (every line must appear in the output):
```
eq_ab=1
eq_ac=1
s_ab=0
s_ac=0
n_ab=1
s_aa2=1
s_a6=0
n_aa2=0
s_long=1
s_df=0
s_dd=1
s_bu=0
s_ci8=1
s_cu8=0
s_a5=0
s_a5u=1
s_ei=0
s_ee=0
s_cc=1
s_pxy=0
s_pxx=1
s_pxu=0
sfx=0 calls=2
```

- [ ] **Step 3.2: Verify it fails.** Run: `bin/madc tests/test3eq.mad` — expected: `unhandled binary operator` error (no tk3Eq lowering yet).

- [ ] **Step 3.3: `binop_overload_symbol` cases.** In `src/cir_builder.cpp` (~4808), after the `tkNotEq` case add:
```cpp
	case TokenID::tk3Eq:    return "===";
	case TokenID::tk3NotEq: return "!==";
```
(This makes the existing `class_operator_call` at :7765 try a user `operator===`/`operator!==` FIRST for class operands — the §2.2 dispatch order — and is exercised in Task 5.)

- [ ] **Step 3.4: `class_operator_call` opsym override.** Change the signature (definition `src/cir_builder.cpp:6108` and its declaration in `src/cir_builder.h` — find it with `grep -n class_operator_call src/cir_builder.h`):
```cpp
node_t CirBuilder::class_operator_call(TokenOperator *top, TokenBase *origin,
				       const char *opsym_override)
```
(declaration gets `const char *opsym_override = NULL`). Inside, both lookups become:
```cpp
		const char *opsym0 = opsym_override
			? opsym_override : binop_overload_symbol(top->id());
```
(at :6128) and
```cpp
	const char *opsym = opsym_override
		? opsym_override : binop_overload_symbol(top->id());
```
(at :6133).

- [ ] **Step 3.5: Declare and add `strict_equality_lowering`.** In `src/cir_builder.h` next to `three_way_builtin_lowering` (~:605):
```cpp
	node_t strict_equality_lowering(class TokenOperator *top,
					TokenBase *origin);
```
Definition in `src/cir_builder.cpp` after `three_way_builtin_lowering` (~:6234 region):
```cpp
// Strict equality `===` / `!==` (STD_MADC dialect): type-domain identity AND
// value equality. Scalars use DataDef::same_representation; class operands
// strict-compare inside the class domain via the SAME operator== dispatch ==
// uses (a user operator===/!== was already tried by class_operator_call at
// the translate_expr binary entry). A statically-false compare still
// evaluates both operands: (l, r, 0|1).
// Spec: docs/superpowers/specs/2026-06-11-strict-equality-design.md §2-3.
node_t CirBuilder::strict_equality_lowering(TokenOperator *top, TokenBase *origin)
{
	bool neq = top->id() == TokenID::tk3NotEq;
	DataDefCLASS *lcls = operand_object_class(top->left);
	DataDefCLASS *rcls = operand_object_class(top->right);
	if (lcls || rcls) {
		// Domain rule: defer to the class's operator== machinery.
		node_t eq = class_operator_call(top, origin, "==");
		if (eq)
			return neq ? node1(N_NOT, eq, origin) : eq;
		if (lcls && lcls == rcls) {
			std::string msg = "no match for strict equality on '"
				+ lcls->name
				+ "' (no operator=== or operator==)";
			return error_node(msg.c_str(), origin);
		}
	} else {
		DataDef *ldd = top->left ? top->left->datadef() : NULL;
		DataDef *rdd = top->right ? top->right->datadef() : NULL;
		if (ldd && rdd && ldd->same_representation(*rdd)) {
			node_t left = translate_expr(top->left);
			node_t right = translate_expr(top->right);
			return node2(neq ? N_NE : N_EQ, left, right, origin);
		}
	}
	// Different domains: constant result, operands still evaluated.
	node_t left = translate_expr(top->left);
	node_t right = translate_expr(top->right);
	return node2(N_COMMA, node2(N_COMMA, left, right, origin),
		     integer(neq ? 1 : 0, origin), origin);
}
```

- [ ] **Step 3.6: Hook into the binary path.** In `src/cir_builder.cpp` directly after the `tk3Way` block (~:7819-7821):
```cpp
			// madc dialect strict equality === / !== — spec
			// docs/superpowers/specs/2026-06-11-strict-equality-design.md.
			// (A user operator===/!== was already dispatched by
			// class_operator_call above.)
			if (tb->id() == TokenID::tk3Eq || tb->id() == TokenID::tk3NotEq)
				return strict_equality_lowering(top, tb);
```

- [ ] **Step 3.7: Build and run.** `( ulimit -t 600; timeout 900 make -C src )` then `bin/madc tests/test3eq.mad` — compare against every `.expect` line.
  - If `uint32_t`/`printf` don't resolve, the embedded auto-include didn't cover them: add `#include <cstdint>` under `#include <cstdio>` (the embedded header set has the C headers; testfreeop.mad proves `<cstdio>`).
  - **If the enum lines fail with 1 instead of 0** (`s_ei`, `s_ee`): the declaration path is dropping `DataDefENUM` identity on enum variables (typing them as plain int). Fix at the deepest layer — the parser's enum-variable declaration must keep the `DataDefENUM *` as the variable's type, not substitute `ddINT`. Find it with `grep -n "DataDefENUM" src/parser.cpp`, trace where an `enum X` variable's `Variable::type` is assigned, and keep the enum DataDef. Re-run `make -C src fulltest` afterwards — enum identity ripples are possible and must be zero.

- [ ] **Step 3.8: Verify EXE parity is unaffected and the suite is green.** Run: `( ulimit -t 1200; timeout 1800 make -C src fulltest )` — expected 575/0/0/18 (572 baseline + test3eq + the two gate tests), exit 0.

- [ ] **Step 3.9: Commit.**
```bash
git add src/cir_builder.h src/cir_builder.cpp tests/test3eq.mad tests/test3eq.expect
git commit -m "feat(cir): ===/!== Tier-1 lowering — representation identity, comma-false, == fallback plumbing"
```

---

### Task 4: Class domain rule — `std::string` + plain-class fallback test

**Files:**
- Create: `tests/test3eqclass.mad`, `tests/test3eqclass.expect`

(The lowering already routes class operands via Task 3; this task PROVES the domain rule on real headers and user classes, before Task 5 adds user `operator===`.)

- [ ] **Step 4.1: Write the test.** `tests/test3eqclass.mad`:
```c
// Strict equality on class operands: the domain rule strict-compares
// through the class's operator== when no user operator=== exists.
// std::string: a literal enters the string domain (spec §2.2).
#include <cstdio>
#include <string>
using namespace std;

class Plain {
public:
	int v;
	Plain(int x) : v(x) {}
	bool operator==(const Plain &o) const { return v == o.v; }
};

int main()
{
	Plain p1(7), p2(7), p3(8);
	printf("p_seq=%d\n", p1 === p2);	// 1: domain rule via operator==
	printf("p_seq2=%d\n", p1 === p3);	// 0
	printf("p_neq=%d\n", p1 !== p2);	// 0: !(===)

	string s("x");
	string s2("x");
	string s3("y");
	printf("s_seq=%d\n", s === s2);		// 1
	printf("s_seq2=%d\n", s === s3);	// 0
	printf("s_lit=%d\n", s === "x");	// 1: literal enters the domain
	printf("s_lit2=%d\n", s === "y");	// 0
	printf("s_neq=%d\n", s !== "x");	// 0
	return 0;
}
```
`tests/test3eqclass.expect`:
```
p_seq=1
p_seq2=0
p_neq=0
s_seq=1
s_seq2=0
s_lit=1
s_lit2=0
s_neq=0
```

- [ ] **Step 4.2: Run it.** `bin/madc tests/test3eqclass.mad` — all lines match. The Plain cases exercise member-`operator==` fallback + `N_NOT`; the string cases exercise the free/external `operator==` machinery under the override. If `s === "x"` fails while plain `s == "x"` works (verify with a 2-line reducer in `tmp/`), the bug is in how `class_operator_call`'s override path reaches `try_free_operator_call` — fix there, not with a string special case.

- [ ] **Step 4.3: Verify the multi-declarator gotcha didn't bite.** `Plain p1(7), p2(7), p3(8);` is the KNOWN parser hang (`Q a(1), b(2);` — known_pre_existing_gaps). If the test hangs, split into three declarations (`Plain p1(7); Plain p2(7); Plain p3(8);`) — do NOT chase the hang in this track.

- [ ] **Step 4.4: Commit.**
```bash
git add tests/test3eqclass.mad tests/test3eqclass.expect
git commit -m "test(cir): === class domain rule — member fallback, std::string literal-enters-domain"
```

---

### Task 5: User `operator===` / `operator!==` overloading + mangling

**Files:**
- Modify: `src/madc_mangle.cpp:147` (`operator_code`)
- Modify: `tests/unit/test_mangle.cpp` (pin the encodings)
- Modify: `tests/test3eqclass.mad` / `.expect` (Money class)

- [ ] **Step 5.1: Write the failing mangler unit test.** In `tests/unit/test_mangle.cpp`, next to the existing operator-mangling TEST_CASEs (match the file's existing CHECK style and any local declaration pattern for `itanium_mangle_operator`):
```cpp
TEST_CASE("Dialect strict-equality operators (Itanium vendor-extended)") {
	// operator=== => v2 (binary vendor op) + source-name "3eq3"
	CHECK(itanium_mangle_operator("Money", "===",
	      std::vector<std::string>{"RK5Money"})
	      == "_ZN5Moneyv23eq3ERK5Money");
	CHECK(itanium_mangle_operator("Money", "!==",
	      std::vector<std::string>{"RK5Money"})
	      == "_ZN5Moneyv23ne3ERK5Money");
}
```

- [ ] **Step 5.2: Verify it fails.** `( ulimit -t 600; timeout 900 make -C src test )` — the new CHECKs fail (`operator_code` returns "" → mangle returns "").

- [ ] **Step 5.3: Add the vendor-extended codes.** In `src/madc_mangle.cpp` `operator_code` (:147), BEFORE the `"=="` line (exact-match equality, but keep the longer spellings first for readability):
```cpp
	// madc dialect operators — Itanium vendor-extended operator-name
	// encoding `v <arity> <source-name>` (no standard code exists).
	if (op == "===") return "v23eq3";
	if (op == "!==") return "v23ne3";
```

- [ ] **Step 5.4: Unit tests green.** `( ulimit -t 600; timeout 900 make -C src test )`.

- [ ] **Step 5.5: Extend the integration test with a Money class.** In `tests/test3eqclass.mad` add above `main` (split declarations per the Task 4.3 gotcha):
```c
// User operator=== dispatches BEFORE the domain rule; explicit user
// operator!== dispatches before the !(===) negation (spec §2.2/§2.4/§2.5).
class Money {
public:
	long amount;
	int currency;
	Money(long a, int c) : amount(a), currency(c) {}
	bool operator==(const Money &o) const { return amount == o.amount; }
	bool operator===(const Money &o) const {
		return amount == o.amount && currency == o.currency;
	}
};
```
and inside `main` (before `return 0;`):
```c
	Money usd5(5, 1);
	Money cad5(5, 2);
	Money usd5b(5, 1);
	printf("m_eq=%d\n", usd5 == cad5);	// 1: loose == ignores currency
	printf("m_seq=%d\n", usd5 === cad5);	// 0: user operator===
	printf("m_seq2=%d\n", usd5 === usd5b);	// 1
	printf("m_neq=%d\n", usd5 !== cad5);	// 1: !== = !(user ===)
```
Append to `tests/test3eqclass.expect`:
```
m_eq=1
m_seq=0
m_seq2=1
m_neq=1
```

- [ ] **Step 5.6: Run it.** `bin/madc tests/test3eqclass.mad` — the four `m_*` lines must match. What this exercises: `parseOperatorId` accepting `Token3Eq` (generic `TokenMultiOp` path — zero parser changes expected), member registration of `operator===`, `binop_overload_symbol`'s `"==="` mapping making `class_operator_call` dispatch it at the :7765 entry, and the `tk3NotEq` path finding NO `operator!==` → falling to `!(operator===)`. If the member name fails to parse, debug `parseOperatorId` (`src/parser.cpp:10178`) — the fix belongs there, generically.

- [ ] **Step 5.7: Sanity-check the emitted method symbol.** Run `bin/madc --emit=c11 tests/test3eqclass.mad` and `grep -c "operator===" ` the output — the default `Money__operator===` scheme flows through the same emission as `Money__operator==`; confirm g++ on the emitted C is NOT required to accept the raw name only if `safe_ident()` already sanitizes it the way it sanitizes `operator==` today. If `--emit=c11` output is un-compilable because of `=` characters in identifiers while `operator==` works, fix in the SAME sanitizer that handles `operator==`, not with a one-off rename.

- [ ] **Step 5.8: Commit.**
```bash
git add src/madc_mangle.cpp tests/unit/test_mangle.cpp tests/test3eqclass.mad tests/test3eqclass.expect
git commit -m "feat(dialect): user operator===/operator!== overloading — vendor-extended mangling v23eq3/v23ne3"
```

---

### Task 6: eval-DSL `!==` support

**Files:**
- Modify: `src/madc_program.cpp` (`is_expression_compare_token` :395; `rewrite_expression_string_compares` ~:516)
- Modify: `tests/unit/test_libmadc_program.cpp` (DSL compare cases)

- [ ] **Step 6.1: Write the failing unit cases.** Find the existing expression-eval DSL tests: `grep -n "eval_expression" tests/unit/test_libmadc_program.cpp | head`. Mirror the file's existing engine/program setup pattern (do NOT invent a new harness) and add a TEST_CASE with these expression/expectation pairs:
  - `"abc" === "abc"` → true; `"abc" !== "abc"` → false; `"abc" !== "abd"` → true
  - `5 === 5` → true; `5 === 6` → false; `5 !== 6` → true
  - `5 === 5.0` → false (int literal vs double literal — different domains)
  - `"5" !== 5` → must produce the DSL error `cannot compare string and non-string values` (mirror however the existing tests assert the `===` mixed-operand error)

- [ ] **Step 6.2: Verify failure.** `( ulimit -t 600; timeout 900 make -C src test )` — the `!==` string cases fail (tk3NotEq not in the compare set → no strcmp rewrite → falls into the new CIR lowering where `const char*` vs `const char*` compares ADDRESSES → wrong value or flaky).

- [ ] **Step 6.3: Implement.** In `src/madc_program.cpp`:
`is_expression_compare_token` (:395) gains:
```cpp
	|| id == TokenID::tk3NotEq;
```
(appended to the existing chain). In `rewrite_expression_string_compares`, after the existing `tk3Eq` replacement block (~:516-530), add:
```cpp
    if ( op->id() == TokenID::tk3NotEq )
    {
	// `!==` on two strings is !(===): strcmp(a,b) != 0.
	TokenNotEq *ne = new TokenNotEq();
	ne->file = op->file;
	ne->line = op->line;
	ne->column = op->column;
	ne->left = cmp;
	ne->right = zero;
	tb = ne;
	return true;
    }
```
Also update the comment above the function (the "(`===` becomes a replacement strcmp(a,b) == 0 node — CIR has no tk3Eq lowering)" parenthetical is now stale — CIR HAS a tk3Eq lowering; the DSL keeps the VALUE-compare rewrite because DSL strings compare by value by spec).

- [ ] **Step 6.4: Unit green.** `( ulimit -t 600; timeout 900 make -C src test )`.

- [ ] **Step 6.5: Commit.**
```bash
git add src/madc_program.cpp tests/unit/test_libmadc_program.cpp
git commit -m "feat(eval-dsl): !== string value-compare rewrite + numeric ===/!== DSL coverage"
```

---

### Task 7: Script-side `array` (madc::value) strict equality — investigate, wire or defer

Script `array` is the builtin `dtARRAY` runtime-object (`cir_builder.cpp:885` — a real `madc::value` buffer via `madarray_*` wrappers), NOT a DataDefCLASS, and `madc::value`'s int/double/bool ctors are `explicit`. Whether `v === 5` can land now depends on whether whole-value scalar ops exist on that surface today.

- [ ] **Step 7.1: Probe.** Create `tmp/value_probe.mad`:
```c
array v;
v = 5;
printf("eq=%d\n", v == 5);
```
Run `bin/madc tmp/value_probe.mad`.

- [ ] **Step 7.2 — Branch A (probe compiles and prints `eq=1`):** whole-value scalar assignment/compare exists. Wire `===`/`!==` for `dtARRAY` operands through the SAME runtime surface `==` uses: locate the `==`-on-dtARRAY lowering (`grep -n "is_array_object" src/cir_builder.cpp`), and route tk3Eq to a strict-compare helper. Since `madc::value::operator==` is ALREADY tag-strict, the strict helper IS the existing equality call — `===` and `==` coincide on two values today (spec §2.6); the difference only shows for `v === 5` vs `v == 5` if `==` lowers the scalar via `as_integer()` coercion rather than a value-wrap. Trace which it is; if `==` coerces, add the runtime helper `extern "C" int __madc_value_strict_eq_long(madc::value *, long)` (+ `_double`, `_cstr` siblings) in `src/madc_mir_backend.cpp` next to the other `madarray_*` wrappers, each implemented as tag-check + compare via the public accessors, and lower `v === <scalar>` to it. Add to `tests/test3eqclass.mad`:
```c
	array v;
	v = 5;
	printf("v_i=%d\n", v === 5);	// 1: integer tag, value 5
	printf("v_d=%d\n", v === 5.0);	// 0: real tag mismatch
	printf("v_n=%d\n", v !== 5);	// 0
```
(+ expect lines `v_i=1`, `v_d=0`, `v_n=0`).

- [ ] **Step 7.2 — Branch B (probe errors):** whole-value scalar ops do not exist on the script surface yet. DEFER: do not build a parallel value-expression machinery inside this track. Record the deferral: add to the spec's Out-of-scope section: "Script-side `array`/`madc::value` `===` lands with the eval package C / script-value-expression work — the static-type and DSL surfaces (where strict compares exist today) are covered." Note it in the CHANGELOG entry (Task 8). The DSL (Task 6) already gives `===`/`!==` strict semantics over dynamic values in `eval_expression`.

- [ ] **Step 7.3: Commit** whichever branch happened (code+tests, or spec+plan note).

---

### Task 8: Full gates + docs sync

- [ ] **Step 8.1: Clean rebuild, warning check.** `( ulimit -t 1200; timeout 1800 make -C src clean )` then `( ulimit -t 1200; timeout 1800 make -C src )` — zero new warnings.
- [ ] **Step 8.2: fulltest.** `( ulimit -t 1800; timeout 2400 make -C src fulltest )` — expected 576+/0/0/18 (572 baseline + test3eq + test3eqclass + 2 gate tests), exit 0, both check gates green.
- [ ] **Step 8.3: Torture regression diff** (the lexer `=`/`!` paths changed): `( ulimit -t 3600; timeout 5400 make -C src gcctest )` then diff the failset against `tmp/failset_lsq.txt` — must be byte-identical (1567 passed).
- [ ] **Step 8.4: SMAUG soak** (C89 — exercises the gated lexer in anger): `cd /workspace/MadSMAUG/runtime/area` then `timeout 50 /workspace/madc/bin/madc --project /workspace/MadSMAUG/compile_commands.json -lcrypt 4000` per the claude_status soak recipe — exit 124 + the `Realms of Despair ready at` line.
- [ ] **Step 8.5: CHANGELOG.** Add under `## [Unreleased]`:
```markdown
### `===` / `!==` strict equality — STD_MADC dialect (feature/strict-equality-claude)

- New dialect operators: `a === b` is type-domain identity AND value
  equality (`uint32_t === int32_t` is false even when values match);
  `!==` is its pure negation. Scalars compare by representation
  (size+signedness+kind; enums and bool are their own domains; pointers
  recurse on the pointee; literals keep their C type — `a === 5u`).
  Class operands dispatch a user `operator===`/`operator!==` first
  (new vendor-extended manglings `v23eq3`/`v23ne3`), then strict-compare
  inside the class domain via `operator==` (`std::string s === "x"`).
  Statically-false compares still evaluate both operands. Tokens are
  STD_MADC-gated (fixes `===` lexing in `--std=c`/`--std=c++` modes);
  the eval-DSL gains the `!==` string value-compare.
  Spec: docs/superpowers/specs/2026-06-11-strict-equality-design.md.
```
(Adjust per Task 7 branch outcome.)
- [ ] **Step 8.6: Mirrors.** Update `docs/test-status.md` counts; update `claude_status.json` head/test_status (REPLACE text, keep it lean); KG note via `scripts/kg_query.sh` if reachable (record sync debt if not); update the spec's status line.
- [ ] **Step 8.7: Commit docs**, then hand back for merge per `superpowers:finishing-a-development-branch` (PR into develop after user verification — end with an explicit "test it now" handoff per the user's workflow).

---

## Self-review notes (already applied)

- Spec §2.1–§2.6 all map to tasks: gating→1, scalar matrix→2+3, domain rule→3+4, user overloads→5, DSL→6, value→7, errors/tests/gates→3.5+8.
- The enum-identity risk (enum vars possibly typed as plain int) is handled in-task (3.7) at the deepest layer, not shimmed.
- `class_operator_call` keeps ONE implementation (override param), no parallel dispatch path.
- Multi-declarator ctor-arg hang (`Q a(1), b(2)`) is a KNOWN pre-existing gap — Task 4.3/5.5 split declarations rather than chase it here.
