# std::string Object Lowering on the CIR Backend — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lower a madc `string` (std::string) variable as a real C++ string *object* on the CIR backend — constructed, destructed at scope exit, passed by reference as a pointer-to-object — matching exactly what g++/clang++ emit (Rule #1), instead of the current broken collapse to `int`.

**Architecture:** madc's sole backend is `parser → cir_node (MC11-IR) → c2mir → MIR`. `cir_builder.cpp` builds the lowered C11 `node_t` tree. A `string` object is a single high-level C++ declaration that fans out (1→N) into lowered C11 nodes — an opaque aligned storage buffer + a constructor call + a scope-exit destructor call — using the **already-present, currently-unused** runtime wrappers in `src/madc_mir_backend.cpp` (`string_construct`, `string_construct_cstr`, `string_destruct`, `string_cstr`, …). Per the MC11-IR "set in stone" rule, every synthesized node carries the *same* originating `TokenDecl` in `cir_node::origin`, so c2mir sees the lowered nodes while madc's high-level view is preserved through the shared provenance.

**Tech Stack:** C++11, c2mir node API (`N_SPEC_DECL`/`N_DECL`/`N_CALL`/`N_CAST`/`N_ARR`, built via the `CirBuilder::node*`/`id`/`str`/`integer`/`simple` helpers), libstdc++ runtime wrappers, doctest unit tests, `.mad` integration tests, `make -C src fulltest`, clang++/g++ as the parity reference.

---

## Background — what is broken and the canon it must match

`bin/madc tests/testlocal.mad` (and `test`, `test4`, `test5`, `testif`, `testprint`, `testversion`, plus most `std::`/STL tests) SIGSEGV. Root cause, confirmed via `bin/madc --emit=c11 tests/testlocal.mad`:

```c
int hello = "Hello, World!";   // WRONG — pointer truncated to 32-bit int
void print(int s);             // WRONG — string& param became int
extern int version;            // WRONG — global string became int
```

`cir_builder.cpp::append_type_specs` has **no `dtSTRING` case**, so a `string` falls through `default: → N_INT` (cir_builder.cpp:290-292) with no pointer declarator. The CIR backend never re-established `std::string` after it became the sole backend (SMAUG is C89 `char*`, so it was out of scope until now).

**Canon (clang++ `-emit-llvm` on `std::string h="x"; print(h);`)** — the target shape:
- `h` is a real stack object (`alloca %"class.std::__cxx11::basic_string"`, 32 bytes, align 8) — **never** a `const char*`.
- constructed via `basic_string(const char*, allocator)`;
- `print(std::string&)` takes a **pointer** to the object (`_Z5printRNSt7...`, `dereferenceable(32)`);
- destructed at scope exit (`...D1Ev`).

**User decision (2026-05-30):** a declared `string` variable is **always** a real object (no `const char*` fast-path). Only a bare string *literal* (`"hi"` as an rvalue) stays `const char*`. See memory `project_cir_stdstring_model`.

**Runtime wrappers already present and unused** (`src/madc_mir_backend.cpp:60-78`):

```c
void *string_construct(void *ptr);                       // placement-new std::string
void *string_construct_cstr(void *ptr, const char *s);   // placement-new std::string(s)
void  string_destruct(void *ptr);                        // ->~basic_string()
void  string_assign(void *dst, void *src);               // *dst = *src
void  string_assign_cstr(void *dst, const char *s);      // *dst = s
const char *string_cstr(void *ptr);                      // ->c_str()
long  string_length(void *ptr);                          // ->length()
void  string_append(void *dst, void *src);               // *dst += *src
void  string_append_cstr(void *dst, const char *s);      // *dst += s
```

These are resolved at MIR-link time via `dlsym(RTLD_DEFAULT, ...)`, exactly like `madc_printstr`/`madc_puts`.

**Stream symbol (canon, verified weak-defined in libstdc++ GLIBCXX_3.4.21):**
`operator<<(ostream&, const std::string&)` =
`_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKNSt7__cxx1112basic_stringIS4_S5_T1_EE`.
It takes `(ostream&, const string&)` and **returns `ostream&`**, so it threads through the existing stream chain unchanged (cir_builder.cpp:710-735).

---

## Scope of THIS plan

This plan delivers **Phase 0 (foundation) + Phase 1 (object decl/construct/destruct-at-scope-end, c-string coercion, stream output)** — a complete, testable slice that makes string objects construct, print, and clean up. It turns the no-parameter trivial cluster green (`testlocal`, and a new focused unit/integration test) and stops the SIGSEGV class.

The remaining phases are **separate plans** (outlined at the end), because each is an independently-testable subsystem and detailing them now would mean guessing at code paths not yet traced:
- **Phase 2** — string parameters (by-ref → `void*`, by-value → copy-temp) + string return values. (Unblocks `testprint`, `test4`, `teststringparam`, …)
- **Phase 3** — methods (`.c_str`/`.length`/`.size`/`.empty`) + operators (`=`, `+=`, `+`, `==`, `!=`). (Unblocks `teststdstringconv`, namespace/STL string use.)
- **Phase 4** — early-exit destruction (destruct injected before `return`/`break`/`continue`/`goto`, and on exception unwind).
- **Phase 5** — reverse-render suppression (`--emit=madc`/`c++` emits the single high-level decl, skips the implied ctor/dtor). Forward-design only until the renderer exists.

---

## File Structure

- **Modify `src/cir_builder.cpp`** — the only translation unit that builds the tree. New helpers + hooks in `append_type_specs`, `var_decl`, `translate_block`, the call-argument lowering, and `ostream_insert_symbol`/`translate_stream_chain`.
- **Modify `include/cir_builder.h`** (the `CirBuilder` class declaration) — declare the new helper methods and the per-scope destruct-tracking. (Confirm exact path; the class is declared in a header included by `cir_builder.cpp`.)
- **Modify `src/cir_node.h`** — steal one byte from `_pad[7]` for a `bool synth_from_origin` marker (Phase 0; consumed by Phase 5's renderer, set now so provenance is correct from the start).
- **Create `tests/teststringobj.mad`** + `tests/teststringobj.expect` — focused integration test for the object lifecycle (decl, construct-from-literal, default-construct, print via `printstr`, `cout <<`).
- **Create `tests/unit/test_cir_string.cpp`** — doctest unit test asserting the emitted C11 for a string decl is the object form (storage + `string_construct_cstr` + `string_destruct`), not `int`. (Confirm the unit harness can invoke `--emit=c11` rendering or the `CirBuilder` directly; if not, fold this assertion into the integration test and skip the unit file.)

---

## Key design decisions (locked here)

1. **Object storage = an 8-aligned word array, not `char[N]`.** `cir_builder.cpp` is itself C++, so it knows `sizeof(std::string)` (32 on this libstdc++) and `alignof(std::string)` (8). Emit storage as `long <name>[NWORDS]` where `NWORDS = (sizeof(std::string) + 7) / 8` (= 4). A `long[]` is naturally 8-aligned and ≥ object size — robust without needing to emit an `__attribute__((aligned))` node. The array name decays to `long*` = the object address.

2. **Every wrapper-call argument that is the object address is wrapped `(void*)`** via an `N_CAST` to `void*`, so c2mir sees `void*`↔`void*` and emits no "without cast" warning. Helper `string_obj_addr(name, origin)` builds `(void*)<name>`.

3. **`translate_expr` of a string-object variable yields the object address** (`id(name)` — the `long[]` decays to `long*`). Use sites decide how to consume it: `string_cstr(addr)` for a `char*` context, the basic_string `operator<<` (which takes the object pointer) for a stream context.

4. **The 1→N expansion happens in `translate_block`**, the one place with the `items` list to append multiple nodes to and the scope boundary for destruct injection. A string-object declaration appends `[storage_decl, ctor_call]` to `items` and registers the variable in a per-scope destruct vector; destruct calls are appended at the end of the block (Phase 1) / at every exit (Phase 4). It must NOT wrap them in an `N_BLOCK` (that would create a nested scope and hide the variable from later statements).

5. **All synthesized nodes share the declaration's `origin`** (`TokenDecl`) and set `synth_from_origin = true`, so c2mir sees the lowered nodes and madc's high-level view survives (MC11-IR invariant).

6. **`string` identification:** a string-object variable is `v->type && v->type->is_string()` and not a pointer. A bare string literal is a `TokenString` → already emitted as `str()` (`const char*`) and is unaffected.

---

## Phase 0 — Foundation: provenance marker + storage/addr helpers

### Task 0.1: Add the `synth_from_origin` provenance marker to `cir_node`

**Files:**
- Modify: `src/cir_node.h:55-61`

- [ ] **Step 1: Add the field, stealing a pad byte**

In `src/cir_node.h`, change the extension-fields block (currently):

```c
	CirSourceLang src_lang;     // which language produced this node
	uint8_t      _pad[7];       // alignment padding
```

to:

```c
	CirSourceLang src_lang;     // which language produced this node
	bool         synth_from_origin; // true: a lowering artifact (e.g. ctor/dtor
				    // call synthesized for a C++ construct). Shares
				    // its origin with the high-level node; the
				    // reverse-renderer (Phase 5) emits the origin
				    // ONCE and suppresses these.
	uint8_t      _pad[6];       // alignment padding
```

- [ ] **Step 2: Verify it compiles and the offset assert still holds**

Run: `make -C src 2>&1 | tail -20`
Expected: builds clean; the `static_assert(offsetof(cir_node, base) == 0, ...)` (cir_node.h:79) is unaffected (we only changed fields after `base`).

- [ ] **Step 3: Commit**

```bash
git add src/cir_node.h
git commit -m "cir_node: add synth_from_origin marker for 1->N C++ lowering provenance"
```

### Task 0.2: Add string-object helpers to CirBuilder

**Files:**
- Modify: `include/cir_builder.h` (CirBuilder class decl — confirm exact path with `grep -rln "class CirBuilder" include src`)
- Modify: `src/cir_builder.cpp` (add helper definitions near the other leaf/decl builders, after `append_type_specs`)

- [ ] **Step 1: Declare the helpers in the class**

Add to the `CirBuilder` class (private section, near `append_type_specs`):

```cpp
	// std::string object lowering (see docs/superpowers/plans/2026-05-30-cir-stdstring-lowering.md)
	static bool is_string_object(DataDef *dd);     // dtSTRING class, not a pointer
	size_t string_obj_words() const;               // ceil(sizeof(std::string)/8)
	node_t string_storage_decl(const char *name, TokenBase *origin);  // long name[NWORDS];
	node_t string_obj_addr(const char *name, TokenBase *origin);      // (void*)name
	node_t void_ptr_type();                         // TYPE node for (void*) casts
	node_t string_ctor_call(const char *name, TokenBase *initexpr, TokenBase *origin);
	node_t string_dtor_call(const char *name, TokenBase *origin);
	void   need_string_runtime(const char *sym, const ExternParam &ret,
				   const std::vector<ExternParam> &params); // extern proto
```

(`ExternParam` and the `need_*` extern-proto machinery already exist — reuse the same pattern as `need_output_extern`/`need_stream_object`. Confirm the exact `ExternParam` shape near cir_builder.cpp:669-730 and the extern-proto emit used by builtins near cir_builder.cpp:1610-1625.)

- [ ] **Step 2: Define `is_string_object` and `string_obj_words`**

In `src/cir_builder.cpp`:

```cpp
#include <string>   // for sizeof(std::string) — add near the top includes if absent

bool CirBuilder::is_string_object(DataDef *dd)
{
	return dd && dd->is_string() && !dd->is_pointer();
}

size_t CirBuilder::string_obj_words() const
{
	return (sizeof(std::string) + sizeof(long) - 1) / sizeof(long);  // = 4
}
```

- [ ] **Step 3: Define the storage decl (`long name[NWORDS];`)**

```cpp
node_t CirBuilder::string_storage_decl(const char *name, TokenBase *origin)
{
	// long <name>[NWORDS];  — 8-aligned opaque buffer for a std::string object.
	node_t spec = list();
	append(spec, simple(N_LONG, origin));
	node_t share = node1(N_SHARE, spec);
	node_t decl_list = list();
	append(decl_list, node3(N_ARR, ignore(), list(),
				integer((long)string_obj_words(), origin)));
	node_t decl = node2(N_DECL, id(name, origin), decl_list);
	node_t sd = simple(N_SPEC_DECL, origin);
	append(sd, share);
	append(sd, decl);
	append(sd, ignore());     // attrs
	append(sd, ignore());     // asm
	append(sd, ignore());     // initializer — none; ctor does it
	CIR_NODE(sd)->synth_from_origin = true;
	return sd;
}
```

- [ ] **Step 4: Define `void_ptr_type` and `string_obj_addr` (the `(void*)name` cast)**

```cpp
node_t CirBuilder::void_ptr_type()
{
	// TYPE_NAME: (LIST(VOID), DECL(IGNORE, LIST(POINTER)))
	node_t spec = list();
	append(spec, simple(N_VOID));
	node_t decl_list = list();
	append(decl_list, pointer());
	return node2(N_TYPE, node1(N_SHARE, spec), node2(N_DECL, ignore(), decl_list));
}

node_t CirBuilder::string_obj_addr(const char *name, TokenBase *origin)
{
	// (void*)<name>  — array name decays to long*, cast to void* (no c2mir warning)
	node_t cast = node2(N_CAST, void_ptr_type(), id(name, origin), origin);
	CIR_NODE(cast)->synth_from_origin = true;
	return cast;
}
```

(Confirm c2mir's `N_CAST` operand order is `(type_name, expr)` — check an existing cast emission in cir_builder.cpp via `grep -n N_CAST src/cir_builder.cpp`. If none exists, verify against `c2m -d` on `(void*)x` and adjust.)

- [ ] **Step 5: Build, then commit (helpers compile even if unused)**

Run: `make -C src 2>&1 | tail -20`
Expected: clean build (unused-function warnings are acceptable here; they're consumed in Phase 1).

```bash
git add include/cir_builder.h src/cir_builder.cpp
git commit -m "cir_builder: add std::string object storage/addr/cast helpers"
```

### Task 0.3: Define ctor/dtor call builders + extern protos

**Files:**
- Modify: `src/cir_builder.cpp`

- [ ] **Step 1: Define `string_ctor_call`**

```cpp
node_t CirBuilder::string_ctor_call(const char *name, TokenBase *initexpr,
				    TokenBase *origin)
{
	// string_construct_cstr((void*)name, <cstr>);  if initexpr is a literal/char*,
	// else string_construct((void*)name);
	node_t args = list();
	append(args, string_obj_addr(name, origin));
	const char *sym;
	if (initexpr) {
		// The initializer's C value must be a `const char *`. A string literal
		// translates to str(); any char*-typed expr is fine. (A `string = string`
		// copy-init is Phase 3; for Phase 1 only literal / char* initializers.)
		append(args, translate_expr(initexpr));
		sym = "string_construct_cstr";
		need_string_runtime(sym, {{N_VOID}, true},
				    {{{N_VOID}, true}, {{N_CHAR}, true}});
	} else {
		sym = "string_construct";
		need_string_runtime(sym, {{N_VOID}, true}, {{{N_VOID}, true}});
	}
	node_t call = node2(N_CALL, id(sym, origin), args, origin);
	CIR_NODE(call)->synth_from_origin = true;
	// A statement needs the call wrapped per the block-item convention used by
	// other expression-statements (confirm: see how translate_stream_chain's
	// N_CALL is returned and appended as a statement in translate_block).
	return call;
}
```

- [ ] **Step 2: Define `string_dtor_call`**

```cpp
node_t CirBuilder::string_dtor_call(const char *name, TokenBase *origin)
{
	node_t args = list();
	append(args, string_obj_addr(name, origin));
	need_string_runtime("string_destruct", {{N_VOID}, false}, {{{N_VOID}, true}});
	node_t call = node2(N_CALL, id("string_destruct", origin), args, origin);
	CIR_NODE(call)->synth_from_origin = true;
	return call;
}
```

- [ ] **Step 3: Define `need_string_runtime`** (mirror `need_output_extern`)

```cpp
void CirBuilder::need_string_runtime(const char *sym, const ExternParam &ret,
				     const std::vector<ExternParam> &params)
{
	// Reuse the existing extern-prototype emit path used for madc_printstr etc.
	// (cir_builder.cpp ~1610-1625). Emit `extern <ret> sym(<params>);` once,
	// guarded by a set membership check like m_output_externs.
	need_output_extern(sym, /*ret*/ ret, params);   // adapt to the real signature
}
```

(EXECUTION NOTE: read `need_output_extern`'s real signature near cir_builder.cpp:719-730 and the builtin-proto table at ~1610-1625 first; `need_string_runtime` is a thin wrapper or a direct reuse. If `need_output_extern` already takes `(sym, ret, params)`, drop the wrapper and call it directly.)

- [ ] **Step 4: Build**

Run: `make -C src 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add src/cir_builder.cpp
git commit -m "cir_builder: add std::string ctor/dtor call builders + extern protos"
```

---

## Phase 1 — Object declaration, scope-end destruction, c-string coercion, stream output

### Task 1.1: Failing integration test for the string-object lifecycle

**Files:**
- Create: `tests/teststringobj.mad`
- Create: `tests/teststringobj.expect`

- [ ] **Step 1: Write the test (no parameters — Phase 1 scope)**

`tests/teststringobj.mad`:

```c
#include <iostream>
using namespace std;

int main()
{
    string a = "alpha";
    string b;                 // default-constructed
    printstr(a);              // char* coercion: string_cstr(a)
    cout << a << endl;        // basic_string operator<<
    return 0;
}
```

`tests/teststringobj.expect`:

```
alpha
alpha
```

- [ ] **Step 2: Run it; verify it FAILS the canonical way**

Run: `bin/madc tests/teststringobj.mad ; echo exit=$?`
Expected: SIGSEGV / non-zero exit (the `int hello = "..."` truncation bug), OR a c2mir type error — confirming the object path doesn't exist yet.

- [ ] **Step 3: Capture the clang++ reference (Rule #1)**

Run:
```bash
printf '#include <iostream>\nusing namespace std;\nint main(){ string a="alpha"; string b; cout<<a<<"\\n"; }\n' > tmp/strobj.cpp
clang++ -std=c++11 tmp/strobj.cpp -o tmp/strobj && tmp/strobj
```
Expected: prints `alpha`. This is the behavior madc must match.

### Task 1.2: Route string-object declarations through the object path in `var_decl`

**Files:**
- Modify: `src/cir_builder.cpp:772-968` (`var_decl`)

- [ ] **Step 1: Early-divert string objects to emit only the storage decl**

At the top of `var_decl` (after `base_dd`/`is_ptr` are computed, before the normal spec/declarator logic), add:

```cpp
	// std::string object: storage is `long name[NWORDS]`; construction and
	// destruction are emitted as separate statements by translate_block (the
	// 1->N C++ lowering). Here we emit ONLY the storage declaration.
	if (is_string_object(v->type)) {
		node_t sd = string_storage_decl(v->name.c_str(), origin);
		return sd;
	}
```

(EXECUTION NOTE: confirm `base_dd`/`is_ptr` are in scope at the chosen insertion point; if `var_decl` is entered before they're set, place this guard right after the function's opening brace using `v->type` directly.)

- [ ] **Step 2: Add the `dtSTRING` case to `append_type_specs` as a safety net**

So any path that still reaches `append_type_specs` with a string type emits `char` (not `int`). In `append_type_specs` (cir_builder.cpp:266-292), before `default:`:

```cpp
	case DataType::dtSTRING:
		// A bare string spec should not normally reach here — string objects
		// use string_storage_decl. Emit `char` so a stray path degrades to a
		// byte type, never a 32-bit-truncating `int`.
		append(lst, simple(N_CHAR));
		break;
```

(Confirm the enum member name is `DataType::dtSTRING` via `grep -n "dtSTRING" include/datadef.h include/datatokens.h`.)

- [ ] **Step 3: Build**

Run: `make -C src 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 4: Inspect emitted C11 — storage decl now correct, ctor/dtor still missing**

Run: `bin/madc --emit=c11 tests/teststringobj.mad`
Expected: `long a[4];` and `long b[4];` appear (no more `int a = "alpha"`). The `string_construct*`/`string_destruct` calls are NOT there yet (Task 1.3), and `printstr`/`cout` args are still raw — so it won't run correctly yet.

- [ ] **Step 5: Commit**

```bash
git add src/cir_builder.cpp
git commit -m "cir_builder: emit std::string objects as aligned storage (was int)"
```

### Task 1.3: Emit ctor after the decl and dtor at scope end in `translate_block`

**Files:**
- Modify: `include/cir_builder.h` (add per-scope tracking type)
- Modify: `src/cir_builder.cpp:1920-1979` (`translate_block`)

- [ ] **Step 1: Add a small struct to track per-scope string objects**

In `include/cir_builder.h`:

```cpp
	struct ScopeStringObj { std::string name; TokenBase *origin; };
```

- [ ] **Step 2: In `translate_block`, emit ctor after each string-object decl and collect for destruct**

In `translate_block`, replace the two decl-emitting sites so that whenever a string-object variable is declared, the ctor call is appended right after the storage decl and the object is recorded.

For the variable loop (cir_builder.cpp:1939-1943):

```cpp
	std::vector<ScopeStringObj> scope_strings;
	for (size_t vi = skip; vi < tc->variables.size(); vi++) {
		Variable *v = tc->variables[vi];
		if (decl_vars.count(v->name)) continue;
		append(items, var_decl(v));
		if (is_string_object(v->type)) {
			// default-constructed: `string b;`
			append(items, string_ctor_call(v->name.c_str(), NULL, v->token_or_block_origin()));
			scope_strings.push_back({ v->name, /*origin*/ NULL });
		}
	}
```

(EXECUTION NOTE: use the block token `tc` as the origin if the variable has no per-decl token here; `var_decl(v)` is called with a NULL origin in this loop today, so pass `tc` for position.)

For the statement loop's `TokenDecl` case — a `TokenDecl` reaches `translate_stmt` which returns the storage decl; intercept it in the statement loop BEFORE calling `translate_stmt`, so the ctor (with the initializer) and tracking are added inline:

```cpp
		TokenDecl *td = dynamic_cast<TokenDecl *>(stb);
		if (td && is_string_object(td->var.type)
		    && (td->var.flags & vfLOCAL)) {
			append(items, var_decl(&td->var, td));     // long name[4];
			TokenBase *initexpr = NULL;
			if (td->initialize) {
				initexpr = td->initialize;
				if (TokenAssign *as = dynamic_cast<TokenAssign *>(initexpr))
					initexpr = as->right;               // unwrap `name = "x"`
			} else if (!td->init_list.empty()) {
				initexpr = td->init_list[0];            // `string s = "x"` literal form
			}
			append(items, string_ctor_call(td->var.name.c_str(), initexpr, td));
			scope_strings.push_back({ td->var.name, td });
			continue;
		}
```

(EXECUTION NOTE: confirm whether `string s = "x"` arrives as `td->initialize` (TokenAssign) or `td->init_list` — set a breakpoint / `--dump-cir`, or print in this branch. The var-decl scalar-init path at cir_builder.cpp:949-958 shows `initialize` is a TokenAssign whose `.right` is the value; mirror that unwrap.)

- [ ] **Step 3: Append destruct calls at block end (reverse declaration order)**

Just before `return node2(N_BLOCK, empty_list, items, tc);` (cir_builder.cpp:1979):

```cpp
	// Destruct scope-local std::string objects at block end, reverse order
	// (matches C++ destruction order). NOTE: early exits (return/break/...) are
	// Phase 4; this covers fall-off-the-end and trailing-return blocks.
	for (auto it = scope_strings.rbegin(); it != scope_strings.rend(); ++it)
		append(items, string_dtor_call(it->name.c_str(), it->origin));
```

- [ ] **Step 4: Build, then inspect emitted C11**

Run: `make -C src 2>&1 | tail -20 && bin/madc --emit=c11 tests/teststringobj.mad`
Expected (shape):
```c
int main(void) {
long a[4];
string_construct_cstr((void*)a, "alpha");
long b[4];
string_construct((void*)b);
printstr(a);                 // still raw — fixed in Task 1.4
... cout ...                 // still raw — fixed in Task 1.5
string_destruct((void*)b);
string_destruct((void*)a);
return 0;
}
```

- [ ] **Step 5: Commit**

```bash
git add include/cir_builder.h src/cir_builder.cpp
git commit -m "cir_builder: construct std::string objects at decl, destruct at scope end"
```

### Task 1.4: Coerce a string object to `const char*` when passed where a char* is expected

**Files:**
- Modify: `src/cir_builder.cpp` (the `TokenCallFunc` argument-lowering loop; locate via `grep -n "func_proto\|N_CALL\|arg" src/cir_builder.cpp` near 1250-1300 and the call-emit path)

- [ ] **Step 1: Wrap string-object args in `string_cstr` for char*/pointer params**

In the argument loop that builds each call argument node: when the parameter's expected type is a pointer/`char*` (i.e. not another `std::string` object) and the argument value's `datadef()` satisfies `is_string_object(...)`, replace the argument node `A` with:

```cpp
	// string object -> const char*  (the dtSTRING->dtCHARptr coercion;
	// AGENTS.md "Key design notes"). string_cstr((void*)objaddr).
	node_t cs_args = list();
	append(cs_args, string_obj_addr(argname, origin));  // (void*)name
	need_string_runtime("string_cstr", {{N_CHAR}, true}, {{{N_VOID}, true}});
	arg_node = node2(N_CALL, id("string_cstr", origin), cs_args, origin);
```

(EXECUTION NOTE: `argname` is the string variable's name; if the argument is not a plain variable (e.g. a temporary), pass `string_obj_addr` of whatever node yields its address. For Phase 1 only plain string-variable args are required — `printstr(a)`. Confirm how the builtin param types are known at the call site: the builtin proto table at ~1610-1625 records `{N_CHAR, true}` for `madc_printstr`/`madc_puts`; key the coercion on that pointer-ness.)

- [ ] **Step 2: Build + inspect**

Run: `make -C src 2>&1 | tail -20 && bin/madc --emit=c11 tests/teststringobj.mad | grep printstr`
Expected: `madc_printstr(string_cstr((void*)a));` (the symbol may be `madc_printstr` after builtin renaming — confirm against the emit).

- [ ] **Step 3: Commit**

```bash
git add src/cir_builder.cpp
git commit -m "cir_builder: coerce std::string objects to const char* at char* call args"
```

### Task 1.5: Stream a string object via the basic_string operator<<

**Files:**
- Modify: `src/cir_builder.cpp:669-689` (`ostream_insert_symbol`), `:710-735` (`translate_stream_chain`)

- [ ] **Step 1: Return the basic_string overload for a string-object value**

In `ostream_insert_symbol`, change the `is_string()` branch (cir_builder.cpp:673) to distinguish an object from a `char*`:

```cpp
	if (is_string_object(dd)) {
		// operator<<(ostream&, const std::string&) — takes the object pointer,
		// returns ostream& (chains). p_out marks the second param as a pointer.
		p_out = {{N_VOID}, true};
		return "_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKNSt7__cxx1112basic_stringIS4_S5_T1_EE";
	}
	if (dd && dd->is_string()) { p_out = {{N_CHAR}, true}; return "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc"; }  // char* / literal — unchanged
```

- [ ] **Step 2: Pass the object address (not c_str) for a string-object stream value**

In `translate_stream_chain` (cir_builder.cpp:727-734), when `vdd` is a string object, the argument must be `(void*)objaddr`, not `translate_expr` (which would also yield the addr for a var, but make it explicit and warning-free):

```cpp
		DataDef *vdd = vals[i]->datadef();
		ExternParam vp;
		const char *sym = ostream_insert_symbol(vdd, vp);
		need_output_extern(sym, true, { { {N_VOID}, true }, vp });
		node_t args = list();
		append(args, result);
		if (is_string_object(vdd)) {
			const char *vn2 = NULL;
			if (TokenVar *tv = dynamic_cast<TokenVar *>(vals[i])) vn2 = tv->var.name.c_str();
			append(args, vn2 ? string_obj_addr(vn2, top) : translate_expr(vals[i]));
		} else {
			append(args, translate_expr(vals[i]));
		}
		result = node2(N_CALL, id(sym), args, top);
```

- [ ] **Step 3: Build + run the integration test**

Run: `make -C src 2>&1 | tail -20 && bin/madc tests/teststringobj.mad ; echo exit=$?`
Expected:
```
alpha
alpha
exit=0
```

- [ ] **Step 4: Confirm c2mir-clean and no truncation warnings**

Run: `bin/madc tests/teststringobj.mad 2>&1 | grep -i "without cast\|warning" ; echo "---"`
Expected: no pointer/integer cast warnings.

- [ ] **Step 5: Commit**

```bash
git add src/cir_builder.cpp
git commit -m "cir_builder: stream std::string objects via basic_string operator<<"
```

### Task 1.6: Verify the trivial cluster + full suite, then commit the integration test

**Files:**
- (validation only) + the new `tests/teststringobj.*` already created

- [ ] **Step 1: Run the no-parameter trivial cluster that depends only on Phase 1**

Run:
```bash
for t in teststringobj testlocal testversion; do echo "== $t =="; bin/madc tests/$t.mad; echo "exit=$?"; done
```
Expected: `teststringobj` and `testlocal` exit 0 with correct output. (`testversion` needs a global string object — if it still fails on the GLOBAL string path, that is a known gap handled by Task 1.7; note it, don't fail the phase.)

- [ ] **Step 2: Run the full suite to check for regressions**

Run: `make -C src fulltest 2>&1 | tail -15`
Expected: integration pass count **≥ 325** (the baseline) and ideally higher (the string-object tests that need only Phase 1 flip green); **no previously-passing test regresses**. Record the new pass/fail numbers.

- [ ] **Step 3: If any regression, triage before proceeding**

For each newly-failing test, run `bin/madc --emit=c11 tests/<name>.mad` and compare to the clang++ reference. Do NOT mask — fix at the deepest layer (Rule #2). Common suspect: a non-`string` test whose variable was mis-detected by `is_string_object` (verify `is_string()` is true ONLY for std::string, not for `char*`).

- [ ] **Step 4: Commit the integration test + status**

```bash
git add tests/teststringobj.mad tests/teststringobj.expect
git commit -m "test: std::string object lifecycle (decl/construct/destruct/print/stream)"
```

### Task 1.7: Global (file-scope) string objects

**Files:**
- Modify: `src/cir_builder.cpp` (the top-level/global var emission path, ~cir_builder.cpp:2260-2290, and global-init handling)

- [ ] **Step 1: Decide global construction strategy and write the failing case**

A file-scope `string version = "v0.0.1";` cannot run a ctor inline (no enclosing block). Match how C++ does it: storage at file scope + construction in a synthesized init sequence run before `main`. For Phase 1, emit the storage as `long version[4];` at file scope and inject `string_construct_cstr((void*)version, "v0.0.1");` as the first statements of `main` (mirrors the existing `global_init_stmts` concept from the old transpiler).

Write `tests/teststringglobal.mad`:

```c
#include <iostream>
using namespace std;
string greeting = "hi";
int main(){ printstr(greeting); return 0; }
```
with `tests/teststringglobal.expect` containing `hi`.

- [ ] **Step 2: Run; confirm it fails (global string still int / unconstructed)**

Run: `bin/madc tests/teststringglobal.mad; echo exit=$?`
Expected: fails (truncation or unconstructed object).

- [ ] **Step 3: Emit global string storage + main-prologue construction**

In the global var path, when `is_string_object(gdd)`: emit `long name[NWORDS];` storage (reuse `string_storage_decl`) and record the (name, initializer) in a `m_global_string_inits` vector. In the `N_FUNC_DEF` emission for `main` (cir_builder.cpp:2030-2047 / translate_block for the main body), prepend `string_construct*` calls for each recorded global. (Destruction of globals at program exit is deferred — C++ would register `__cxa_atexit`; not needed for test parity and noted as a follow-up.)

- [ ] **Step 4: Build + run both global tests**

Run: `make -C src 2>&1 | tail -5 && bin/madc tests/teststringglobal.mad && bin/madc tests/testversion.mad`
Expected: both print correctly, exit 0.

- [ ] **Step 5: fulltest + commit**

Run: `make -C src fulltest 2>&1 | tail -15`
Expected: pass count ≥ Task 1.6's number; no regressions.

```bash
git add src/cir_builder.cpp tests/teststringglobal.mad tests/teststringglobal.expect
git commit -m "cir_builder: file-scope std::string objects (storage + main-prologue construct)"
```

---

## Validation (end of Phase 1)

- `make -C src fulltest` — integration pass count strictly greater than the 325 baseline, zero regressions, unit tests green.
- `bin/madc tests/teststringobj.mad` and `tests/teststringglobal.mad` — correct output, exit 0, no "without cast" warnings.
- Spot-check 3 string-using tests still failing → confirm they fail only on Phase 2/3 features (params/methods/operators), not on the Phase 1 object model.
- Update `claude_status.json`, `docs/test-status.md` headers, and KG (Gap `cir_stdstring_lowering`) with the new baseline.

---

## Subsequent plans (separate documents, not detailed here)

Each is its own `docs/superpowers/plans/2026-05-DD-*.md`, produced when Phase 1 lands:

- **Phase 2 — string parameters & returns.** `param_decl` (cir_builder.cpp:460): a `string&`/`string` param → `void*` (pointer to object); call site passes the object address for by-ref, and for by-value copy-constructs a temp object (`string_construct` + `string_assign`) destructed after the call; string return values allocate a caller-side object (NRVO-style) or return via the existing struct/`__retbuf` path. Unblocks `testprint`, `test4`, `test5`, `teststringparam`.
- **Phase 3 — methods & operators.** `s.c_str()`→`string_cstr`, `.length()/.size()`→`string_length`, `.empty()`→`string_length(...)==0`; `s = "x"`→`string_assign_cstr`, `s = t`→`string_assign`, `s += ...`→`string_append(_cstr)`, `s + t`→temp + append; `s == t`/`!=`→`strcmp(string_cstr(s),string_cstr(t)) [==|!=] 0`. Unblocks `teststdstringconv` and the namespace/STL string surface.
- **Phase 4 — early-exit destruction.** Inject `string_destruct` before every `return`/`break`/`continue`/`goto` that leaves a scope holding live string objects, and on exception unwind. Requires threading the per-scope object list into `translate_return`/`translate_*`. Correctness for RAII.
- **Phase 5 — reverse-render suppression.** When the `--emit=madc`/`--std=c++`/`.mc11` renderer is built, emit the single high-level `string s = "x"` from the shared `origin` and skip nodes with `synth_from_origin == true`. Forward-design only (marker already set from Phase 0).

---

## Self-Review

- **Spec coverage:** the user's model — "a string is a class object; literals stay literal; by-ref = pointer-to-object; match g++" — maps to: object storage + ctor/dtor (1.2/1.3/1.7), literal stays `const char*` (untouched `TokenString`/str()), char* coercion (1.4), stream overload (1.5); by-ref/methods/operators explicitly deferred to Phases 2/3 with insertion points. MC11-IR provenance (shared `origin`, `synth_from_origin`) covered in 0.1 and applied in every synth builder.
- **Placeholder scan:** no "TBD/handle edge cases" steps; every code step shows code. Items marked "EXECUTION NOTE" are explicit *verify-against-real-source* checks (exact enum name, `N_CAST` operand order, `need_output_extern` signature, initializer storage form), not hand-waves — they exist because those exact shapes must be confirmed in-tree, and each says what to confirm and where.
- **Type consistency:** helper names are stable across tasks (`is_string_object`, `string_obj_addr`, `string_storage_decl`, `string_ctor_call`, `string_dtor_call`, `need_string_runtime`, `string_obj_words`); runtime symbols match `madc_mir_backend.cpp` exactly; `synth_from_origin` named identically in 0.1 and all consumers.
- **Risk note:** the largest unknowns are (a) `need_output_extern`'s exact signature and the `ExternParam` literal shape, and (b) whether `string s="x"` arrives via `td->initialize` vs `td->init_list`. Both are pinned by reading 3 specific code spans before coding, called out inline. If `--emit=c11` can't be asserted from the doctest harness, the unit test folds into the integration test (noted in File Structure).
