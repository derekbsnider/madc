# Phase-2 CIR Stream I/O Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `--backend=cir` handle program I/O — the `puti`-family stdout builtins and `cout`/`cerr`/`clog`/`cin` stream operators — moving past the 171/419 baseline (streams are the dominant `c2mir_rejected` cause).

**Architecture:** In `CirBuilder` (`src/cir_builder.cpp`), (A) map builtin output-fn calls to their `madc_*` runtime symbols, and (B) detect `<<`/`>>` operator chains rooted at a stream identifier and lower them to **direct Itanium-mangled libstdc++ `std::ostream::operator<<` / `std::istream::operator>>` calls** on the real stream objects (`_ZSt4cout` etc.). Both paths need hand-built `extern` prototypes (resolved via `dlsym(RTLD_DEFAULT)` against the loaded libstdc++/runtime). Verified against `g++ -S`-derived symbols.

**Tech Stack:** C++11, c2mir node API, doctest (`tests/unit/test_cir.cpp`), `c++filt` for demangle verification.

**Spec:** `docs/superpowers/specs/2026-05-29-cir-phase2-output-design.md`

## Reference facts (verified during design)

- Builtin → runtime map (`madc_emit_c.cpp:710-720, 5633`): `puti`→`madc_puti(int64_t)`, `putu`→`madc_putu(uint64_t)`, `putd`→`madc_putd(double)`, `putf`→`madc_putf(float)`, `printstr`→`madc_printstr(const char*)`. (`puts`→`madc_puts` also exists; include it.)
- ostream `operator<<` (all ABI shape `void* f(void* os, value)`): int `_ZNSolsEi`, long `_ZNSolsEl`, unsigned `_ZNSolsEj`, double `_ZNSolsEd`, bool `_ZNSolsEb`, void* `_ZNSolsEPKv`; non-member const char* `_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc`, char `_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c`, std::string `_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKNSt7__cxx1112basic_stringIS4_S5_T1_EE`.
- `endl`: manipulator `_ZNSolsEPFRSoS_E(os, &endl)`; endl fn `_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_`.
- istream `operator>>` (ABI shape `void* f(void* is, void* &var)`): int `_ZNSirsERi`, long `_ZNSirsERl`, unsigned `_ZNSirsERj`, double `_ZNSirsERd`; non-member char `_ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_RS3_`, std::string `_ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE`.
- Objects: `cout`=`_ZSt4cout`, `cerr`=`_ZSt4cerr`, `clog`=`_ZSt4clog`, `cin`=`_ZSt3cin` (address-taken as first arg).
- CIR sites: binary-op block `src/cir_builder.cpp:824-866` (`tkBSL`→`N_LSH` at :848, `tkBSR`→`N_RSH` at :849); call block at :869-877 (`referenced_funcs.insert` + `id(name)` + `N_CALL`); extern-proto pass at :1289-1350 (funcdef_map only — does NOT cover these symbols). Type-spec codes via `simple(N_INT/N_LONG/N_UNSIGNED/N_CHAR/N_VOID/N_DOUBLE/N_FLOAT)`. `pointer()` builds `N_POINTER`.
- `cout`/`endl` reach `translate_expr` as an undeclared identifier — either a `TokenVar` (`tv->var.name`) or `TokenIdent` (`ti->str`). Handle both.

---

## Task 0: stdout-capture test helper

Output tests need to assert what the program prints. Add a helper that captures fd 1 around the MIR interpret call (covers both `madc_puti`/`printf` and `std::cout`, same fd).

**Files:** Modify `tests/unit/test_cir.cpp` (near `cir_run_builder`).

- [ ] **Step 1: Add includes (top of file, with the other includes)**

```cpp
#include <unistd.h>
#include <iostream>
```
(If `<iostream>` is already included, skip it.)

- [ ] **Step 2: Add `cir_capture` after `cir_run_builder`**

```cpp
// Like cir_run_builder, but captures everything the program writes to stdout
// (fd 1) during execution and returns it. Covers madc_puti/printf AND std::cout.
static std::string cir_capture(const char *source) {
    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));

    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);

    CirBuilder builder(c2m);
    node_t tree = builder.translate_module(prog.get());
    REQUIRE(tree != nullptr);
    REQUIRE(cir_report_errors(stderr, tree) == 0);
    REQUIRE(cir_compile(mir_ctx, c2m, tree, "test_mod") == 1);

    MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(mir_ctx));
    MIR_load_module(mir_ctx, mod);
    MIR_link(mir_ctx, MIR_set_interp_interface, NULL);

    MIR_item_t func_item = NULL;
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, mod->items); it; it = DLIST_NEXT(MIR_item_t, it))
        if (it->item_type == MIR_func_item && strcmp(it->u.func->name, "main") == 0)
            func_item = it;
    REQUIRE(func_item != nullptr);

    // Redirect fd 1 to a temp file around the interp call.
    fflush(stdout);
    int saved = dup(1);
    char tmpl[] = "/tmp/cir_capXXXXXX";
    int tfd = mkstemp(tmpl);
    dup2(tfd, 1);

    MIR_val_t val;
    MIR_interp(mir_ctx, func_item, &val, 0);

    std::cout.flush();   // flush the libstdc++ std::cout buffer (same object the JIT used)
    fflush(stdout);
    dup2(saved, 1);
    close(saved);

    lseek(tfd, 0, SEEK_SET);
    std::string out;
    char buf[512]; ssize_t n;
    while ((n = read(tfd, buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(tfd);
    unlink(tmpl);

    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);
    return out;
}
```

- [ ] **Step 3: Add a smoke test (uses an already-working construct — a return, no output yet)**

```cpp
TEST_CASE("CirBuilder: capture helper baseline") {
    // No output yet; just verify the helper runs a program and returns "".
    CHECK(cir_capture("int main() { return 0; }") == "");
}
```

- [ ] **Step 4: Build and run**

Run: `make -C src test 2>&1 | grep -iE "capture helper|FAILED|test cases" | head`
Expected: builds; "capture helper baseline" passes (empty output).

- [ ] **Step 5: Commit**

```bash
git add tests/unit/test_cir.cpp
git commit -m "test(cir): add cir_capture stdout-capturing helper for I/O tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 1: Part A — stdout print builtins + the extern-proto infrastructure

Map `puti`-family calls to `madc_*` runtime symbols and emit their externs. This also builds the reusable extern-proto emitter that Part B uses.

**Files:** `src/cir_builder.h`, `src/cir_builder.cpp`, `tests/unit/test_cir.cpp`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("CirBuilder: stdout print builtins") {
    CHECK(cir_capture("int main() { puti(42); return 0; }") == "42");
    CHECK(cir_capture("int main() { printstr(\"hi\"); return 0; }") == "hi");
}
```

- [ ] **Step 2: Run, expect FAIL**

Run: `make -C src test 2>&1 | grep -iE "stdout print|FAILED|undefined" | head`
Expected: FAIL — `puti`/`printstr` emitted as bare calls → `import of undefined item puti` (link error → REQUIRE trips or output wrong).

- [ ] **Step 3: Declare the new members in `src/cir_builder.h`**

In the class body (near the other declaration builders), add:

```cpp
	// ---- Output (Phase-2) ----
	// A param of an output extern: its type-spec node codes + whether it is a pointer.
	struct ExternParam { std::vector<c2mir_node_code_t> specs; bool ptr; };
	// Record (once) an extern proto for an output runtime/libstdc++ symbol.
	// ret_ptr=true -> returns void*, else void.
	void need_output_extern(const char *symbol, bool ret_ptr,
				const std::vector<ExternParam> &params);
	// Map a builtin print-fn name to its madc_* runtime symbol ("" if not one).
	static const char *builtin_output_runtime(const std::string &name);
```

And add a member to hold the recorded protos (near other members, e.g. beside `referenced_funcs`):

```cpp
	std::map<std::string, node_t> m_output_externs; // symbol -> proto SPEC_DECL (dedup)
```
(Ensure `<map>`, `<vector>`, `<string>` are included by the header or its includes — `cir_builder.h` already uses STL; add `#include <map>`/`<vector>` if not present.)

- [ ] **Step 4: Implement the extern-proto emitter in `src/cir_builder.cpp`** (place near `param_decl`/`var_decl`)

```cpp
const char *CirBuilder::builtin_output_runtime(const std::string &name)
{
	if (name == "puti")     return "madc_puti";
	if (name == "putu")     return "madc_putu";
	if (name == "putd")     return "madc_putd";
	if (name == "putf")     return "madc_putf";
	if (name == "puts")     return "madc_puts";
	if (name == "printstr") return "madc_printstr";
	return "";
}

void CirBuilder::need_output_extern(const char *symbol, bool ret_ptr,
				    const std::vector<ExternParam> &params)
{
	if (m_output_externs.count(symbol)) return;

	// SHARE(LIST(EXTERN, <ret-specs>))  — ret is void (+ pointer in decl if ret_ptr)
	node_t ext_list = list();
	append(ext_list, simple(N_EXTERN));
	append(ext_list, simple(N_VOID));
	node_t share = node1(N_SHARE, ext_list);

	// Parameter list of N_TYPE(LIST(specs), DECL(IGNORE, LIST([pointer])))
	node_t param_list = list();
	if (params.empty()) {
		node_t void_spec = node1(N_LIST, simple(N_VOID));
		node_t void_decl = node2(N_DECL, ignore(), list());
		append(param_list, node2(N_TYPE, void_spec, void_decl));
	} else {
		for (size_t i = 0; i < params.size(); i++) {
			node_t specs = list();
			for (size_t j = 0; j < params[i].specs.size(); j++)
				append(specs, simple(params[i].specs[j]));
			node_t pdecl_list = list();
			if (params[i].ptr) append(pdecl_list, pointer());
			node_t pdecl = node2(N_DECL, ignore(), pdecl_list);
			append(param_list, node2(N_TYPE, specs, pdecl));
		}
	}

	node_t func_inner = node1(N_FUNC, param_list);
	node_t decl_list = list();
	append(decl_list, func_inner);
	if (ret_ptr) append(decl_list, pointer());   // returns void*
	node_t decl = node2(N_DECL, id(symbol), decl_list);

	node_t proto = simple(N_SPEC_DECL);
	append(proto, share);
	append(proto, decl);
	append(proto, ignore());
	append(proto, ignore());
	append(proto, ignore());
	m_output_externs[symbol] = proto;
}
```

- [ ] **Step 5: Map builtin calls in the call block.** In `translate_expr`'s `ttCallFunc` case (`src/cir_builder.cpp:870-877`), replace the start of the block so a builtin name routes to its runtime symbol + records the extern:

Find:
```cpp
		if (tcf) {
			referenced_funcs.insert(tcf->var.name);
			node_t func_id = id(tcf->var.name.c_str(), tb);
```
Replace with:
```cpp
		if (tcf) {
			const char *rt = builtin_output_runtime(tcf->var.name);
			if (rt[0]) {
				// puti/putu/putd/putf/puts/printstr -> madc_* (stdout)
				static const std::map<std::string, ExternParam> sigs = {
					{"madc_puti",     {{N_LONG}, false}},
					{"madc_putu",     {{N_UNSIGNED, N_LONG}, false}},
					{"madc_putd",     {{N_DOUBLE}, false}},
					{"madc_putf",     {{N_FLOAT}, false}},
					{"madc_puts",     {{N_CHAR}, true}},
					{"madc_printstr", {{N_CHAR}, true}},
				};
				need_output_extern(rt, false, { sigs.at(rt) });
				node_t a = list();
				for (size_t i = 0; i < tcf->parameters.size(); i++)
					append(a, translate_expr(tcf->parameters[i]));
				return node2(N_CALL, id(rt, tb), a, tb);
			}
			referenced_funcs.insert(tcf->var.name);
			node_t func_id = id(tcf->var.name.c_str(), tb);
```

- [ ] **Step 6: Emit the recorded output externs in `translate_module`.** Right before `// Pass 1: Forward declarations` (`src/cir_builder.cpp:1352`), add:

```cpp
		// Pass 0.8: output externs (madc_* builtins, libstdc++ stream symbols)
		for (auto &kv : m_output_externs)
			append(top_list, kv.second);
```

- [ ] **Step 7: Run, expect PASS**

Run: `make -C src test 2>&1 | grep -iE "stdout print|FAILED" | head`
Expected: both assertions pass (`puti(42)`→`"42"`, `printstr("hi")`→`"hi"`).

- [ ] **Step 8: Full suite + triage**

Run: `make -C src fulltest 2>&1 | tail -3`  (expect 419 passed, 0 failed)
Run: `bash tmp/cir_triage.sh 2>/dev/null | sed -n '1,8p'`  (record; `import of undefined item puti/putu/...` should drop ~24)

- [ ] **Step 9: Commit**

```bash
git add src/cir_builder.h src/cir_builder.cpp tests/unit/test_cir.cpp
git commit -m "feat(cir): map puti-family builtins to madc_* runtime + extern protos

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Part B PoC — `cout << int`

Prove the mangled-libstdc++ approach end-to-end on the simplest case before building the full table.

**Files:** `src/cir_builder.h`, `src/cir_builder.cpp`, `tests/unit/test_cir.cpp`.

- [ ] **Step 1: Confirm the `cout` token class (investigation, no code yet)**

Add a temporary debug line at the top of the binary-op block to learn the class, OR reason from the existing identifier handling. Quick check:
Run: `printf 'int main(){ cout << 5; return 0; }\n' > tmp/p.mad`
Run: `LD_LIBRARY_PATH=lib:/usr/local/lib bin/madc --backend=cir tmp/p.mad 2>&1 | head -3`
The "undeclared identifier cout" confirms `cout` reaches CIR as an identifier. The helper below handles BOTH `TokenVar` and `TokenIdent`, so exact class isn't blocking; note which one it is for later tasks if easily visible.

- [ ] **Step 2: Write the failing test**

```cpp
TEST_CASE("CirBuilder: cout << int (PoC)") {
    CHECK(cir_capture("int main() { cout << 5; return 0; }") == "5");
}
```

- [ ] **Step 3: Run, expect FAIL**

Run: `make -C src test 2>&1 | grep -iE "cout << int|FAILED|undeclared" | head`
Expected: FAIL — `cout << 5` becomes `N_LSH(cout, 5)` → undeclared `cout`.

- [ ] **Step 4: Declare stream helpers in `src/cir_builder.h`**

```cpp
	// ---- Streams (Phase-2) ----
	enum StreamKind { SK_NONE, SK_COUT, SK_CERR, SK_CLOG, SK_CIN };
	StreamKind stream_ident_kind(TokenBase *tb);          // cout/cerr/clog/cin or none
	static const char *stream_object_symbol(StreamKind k); // _ZSt4cout etc.
	node_t translate_stream_chain(TokenOperator *top, StreamKind k, bool is_out);
```

- [ ] **Step 5: Implement the identifier + object helpers in `src/cir_builder.cpp`**

```cpp
CirBuilder::StreamKind CirBuilder::stream_ident_kind(TokenBase *tb)
{
	std::string name;
	if (TokenVar *tv = dynamic_cast<TokenVar *>(tb)) name = tv->var.name;
	else if (TokenIdent *ti = dynamic_cast<TokenIdent *>(tb)) name = ti->str;
	else return SK_NONE;
	if (name == "cout") return SK_COUT;
	if (name == "cerr") return SK_CERR;
	if (name == "clog") return SK_CLOG;
	if (name == "cin")  return SK_CIN;
	return SK_NONE;
}

const char *CirBuilder::stream_object_symbol(StreamKind k)
{
	switch (k) {
	case SK_COUT: return "_ZSt4cout";
	case SK_CERR: return "_ZSt4cerr";
	case SK_CLOG: return "_ZSt4clog";
	case SK_CIN:  return "_ZSt3cin";
	default:      return "_ZSt4cout";
	}
}
```

- [ ] **Step 6: Implement a minimal `translate_stream_chain` (int-only for the PoC)** in `src/cir_builder.cpp`

```cpp
node_t CirBuilder::translate_stream_chain(TokenOperator *top, StreamKind k, bool is_out)
{
	// Collect chain values left-to-right: walk left while it is a same-direction
	// stream operator; the leftmost leaf is the stream identifier (skipped).
	std::vector<TokenBase *> vals;
	TokenBase *node = top;
	std::vector<TokenOperator *> ops;
	while (TokenOperator *o = dynamic_cast<TokenOperator *>(node)) {
		if (stream_ident_kind(o->left) == SK_NONE &&
		    !dynamic_cast<TokenOperator *>(o->left)) break;
		ops.push_back(o);
		node = o->left;
		if (stream_ident_kind(node) != SK_NONE) break;
	}
	// ops are outer..inner; reverse to inner..outer so values go left-to-right.
	for (size_t i = ops.size(); i-- > 0; )
		vals.push_back(ops[i]->right);

	node_t result = node1(N_ADDR, id(stream_object_symbol(k)));
	// PoC: int only.  _ZNSolsEi(os, int) -> os
	need_output_extern("_ZNSolsEi", true, { { {N_INT}, false } });
	for (size_t i = 0; i < vals.size(); i++) {
		node_t v = translate_expr(vals[i]);
		node_t args = list();
		append(args, result);
		append(args, v);
		result = node2(N_CALL, id("_ZNSolsEi"), args, top);
	}
	return result;
}
```
Also declare the stream-object extern (it's an object, not a function). Add a tiny object-extern emitter — add to `need_output_extern`'s neighborhood OR inline here. Simplest: emit it via a dedicated map too. Add this method (declare in header `void need_stream_object(StreamKind k);` and a `std::set<std::string> m_stream_objects;` member, appended in Pass 0.8):

```cpp
void CirBuilder::need_stream_object(StreamKind k)
{
	const char *sym = stream_object_symbol(k);
	if (m_stream_objects.count(sym)) return;
	m_stream_objects.insert(sym);
	// extern char SYM;   (address-taken; type is opaque)
	node_t share = node1(N_SHARE, [&]{ node_t l = list();
		append(l, simple(N_EXTERN)); append(l, simple(N_CHAR)); return l; }());
	node_t decl = node2(N_DECL, id(sym), list());
	node_t sd = simple(N_SPEC_DECL);
	append(sd, share); append(sd, decl);
	append(sd, ignore()); append(sd, ignore()); append(sd, ignore());
	m_stream_object_protos.push_back(sd);
}
```
(Add members `std::set<std::string> m_stream_objects;` and `std::vector<node_t> m_stream_object_protos;` to the header; in Pass 0.8 also `for (node_t p : m_stream_object_protos) append(top_list, p);`. Call `need_stream_object(k)` at the top of `translate_stream_chain`.)

NOTE: if the C++11 lambda-in-arg above is awkward, build the share list with plain statements instead — same nodes.

- [ ] **Step 7: Hook stream detection into the binary-op block.** In `translate_expr`, at the START of the binary block (`src/cir_builder.cpp:825`, right after `if (top && top->left && top->right) {` and BEFORE `node_t left = translate_expr(top->left);`), add:

```cpp
			// Stream chain? cout/cerr/clog << ... or cin >> ...
			if (tb->id() == TokenID::tkBSL || tb->id() == TokenID::tkBSR) {
				bool is_out = (tb->id() == TokenID::tkBSL);
				// find leftmost leaf
				TokenBase *leaf = top->left;
				while (TokenOperator *o = dynamic_cast<TokenOperator *>(leaf)) {
					if (o->id() != tb->id()) break;
					leaf = o->left;
				}
				StreamKind k = stream_ident_kind(leaf);
				bool ostream_k = (k == SK_COUT || k == SK_CERR || k == SK_CLOG);
				if ((is_out && ostream_k) || (!is_out && k == SK_CIN))
					return translate_stream_chain(top, k, is_out);
			}
```

- [ ] **Step 8: Run, expect PASS**

Run: `make -C src test 2>&1 | grep -iE "cout << int|FAILED" | head`
Expected: `cout << 5` prints `"5"`.

- [ ] **Step 9: Demangle check + full suite**

Run: `echo _ZNSolsEi | c++filt`  → Expected: `std::ostream::operator<<(int)` (confirms the symbol round-trips to real C++).
Run: `make -C src fulltest 2>&1 | tail -3`  (expect 419 passed, 0 failed)

- [ ] **Step 10: Commit**

```bash
git add src/cir_builder.h src/cir_builder.cpp tests/unit/test_cir.cpp
git commit -m "feat(cir): PoC cout<<int via mangled std::ostream::operator<<(int)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Part B — full ostream overload table + chains + cerr/clog

Generalize the PoC: pick the `operator<<` symbol by each value's type, support multi-value chains and `cerr`/`clog`.

**Files:** `src/cir_builder.h`, `src/cir_builder.cpp`, `tests/unit/test_cir.cpp`.

- [ ] **Step 1: Write failing tests**

```cpp
TEST_CASE("CirBuilder: ostream chains and types") {
    CHECK(cir_capture("int main() { cout << \"x=\" << 5 << '!'; return 0; }") == "x=5!");
    CHECK(cir_capture("int main() { double d = 1.5; cout << d; return 0; }") == "1.5");
    CHECK(cir_capture("int main() { cout << \"a\"; cerr << \"b\"; return 0; }") == "a");
}
```
(The `cerr` text goes to fd 2, not captured — so the first assertion's expected stdout is just `"a"`. This confirms cerr routes to cerr, not stdout.)

- [ ] **Step 2: Run, expect FAIL** (only int currently handled; char*/char/double fall through wrongly or assert)

Run: `make -C src test 2>&1 | grep -iE "ostream chains|FAILED" | head`

- [ ] **Step 3: Add the ostream overload selector in `src/cir_builder.cpp`**

```cpp
// Returns {symbol, ret_ptr=true, value-extern-param}. Selides on the value's type.
const char *CirBuilder::ostream_insert_symbol(DataDef *dd, ExternParam &p_out)
{
	bool is_ptr = dd && dd->is_pointer();
	DataType dt = dd ? dd->rawtype() : DataType::dtINT64;
	if (dd && dd->is_string()) { p_out = {{N_CHAR}, true};  return "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc"; }
	if (is_ptr && dt == DataType::dtCHAR) { p_out = {{N_CHAR}, true}; return "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc"; }
	if (is_ptr) { p_out = {{N_VOID}, true}; return "_ZNSolsEPKv"; }
	switch (dt) {
	case DataType::dtCHAR:   p_out = {{N_CHAR}, false};  return "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c";
	case DataType::dtDOUBLE:
	case DataType::dtFLOAT:  p_out = {{N_DOUBLE}, false}; return "_ZNSolsEd";
	case DataType::dtINT16:
	case DataType::dtINT32:  p_out = {{N_INT}, false};    return "_ZNSolsEi";
	case DataType::dtUINT8:
	case DataType::dtUINT16:
	case DataType::dtUINT32: p_out = {{N_UNSIGNED, N_INT}, false}; return "_ZNSolsEj";
	case DataType::dtUINT64: p_out = {{N_UNSIGNED, N_LONG}, false}; return "_ZNSolsEl"; // long overload ok for 64-bit
	case DataType::dtINT64:
	default:                 p_out = {{N_LONG}, false};   return "_ZNSolsEl";
	}
}
```
Declare in header: `const char *ostream_insert_symbol(DataDef *dd, ExternParam &p_out);`
Note `float` is promoted to `double` in varargs/ostream, and `operator<<(double)` is the right overload — pass the value through `translate_expr`; if the value is float-typed, c2mir will promote on the call since the param is `double`. (If a float value mis-narrows, wrap with a cast to double — verify via the `double d=1.5` test which already exercises the double path.)

- [ ] **Step 4: Use the selector in `translate_stream_chain` (replace the PoC int-only body of the value loop)**

Replace the int-only loop in `translate_stream_chain` with:
```cpp
	for (size_t i = 0; i < vals.size(); i++) {
		if (is_out) {
			DataDef *vdd = vals[i]->datadef();
			ExternParam vp;
			const char *sym = ostream_insert_symbol(vdd, vp);
			need_output_extern(sym, true, { { {N_VOID}, true }, vp });
			node_t v = translate_expr(vals[i]);
			node_t args = list();
			append(args, result);
			append(args, v);
			result = node2(N_CALL, id(sym), args, top);
		}
	}
```
(`datadef()` is `TokenBase`'s type accessor; if a value lacks one, it defaults to the int path. The first extern param `{ {N_VOID}, true }` is the `ostream*` first argument.)

- [ ] **Step 5: Run, expect PASS**

Run: `make -C src test 2>&1 | grep -iE "ostream chains|cout << int|FAILED" | head`
Expected: pass (including the PoC test still green).

- [ ] **Step 6: Demangle spot-check + full suite + triage**

Run: `printf '%s\n%s\n%s\n' _ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc _ZNSolsEd _ZNSolsEl | c++filt`
Expected: `... operator<<(... const char*)`, `std::ostream::operator<<(double)`, `std::ostream::operator<<(long)`.
Run: `make -C src fulltest 2>&1 | tail -3` (expect 419/0)
Run: `bash tmp/cir_triage.sh 2>/dev/null | sed -n '1,8p'` (record; `undeclared identifier cout` + `shift operands` should drop sharply)

- [ ] **Step 7: Commit**

```bash
git add src/cir_builder.h src/cir_builder.cpp tests/unit/test_cir.cpp
git commit -m "feat(cir): full ostream operator<< overload table + chains + cerr/clog

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: Part B — `endl` manipulator

**Files:** `src/cir_builder.cpp` (+ header if needed), `tests/unit/test_cir.cpp`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("CirBuilder: cout endl") {
    CHECK(cir_capture("int main() { cout << 5 << endl; return 0; }") == "5\n");
}
```

- [ ] **Step 2: Run, expect FAIL** (`endl` translated as an undeclared identifier value)

Run: `make -C src test 2>&1 | grep -iE "cout endl|FAILED" | head`

- [ ] **Step 3: Handle `endl` in the ostream value loop.** In `translate_stream_chain`'s output branch, before the normal `ostream_insert_symbol` path, detect an `endl` identifier value and emit the manipulator call:

```cpp
		if (is_out) {
			// endl manipulator: _ZNSolsEPFRSoS_E(os, &endl)
			std::string vn;
			if (TokenVar *tv = dynamic_cast<TokenVar *>(vals[i])) vn = tv->var.name;
			else if (TokenIdent *ti = dynamic_cast<TokenIdent *>(vals[i])) vn = ti->str;
			if (vn == "endl") {
				const char *MANIP = "_ZNSolsEPFRSoS_E";
				const char *ENDLF = "_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_";
				need_output_extern(MANIP, true, { { {N_VOID}, true }, { {N_VOID}, true } });
				need_output_extern(ENDLF, true, { { {N_VOID}, true } }); // fn; address taken
				node_t args = list();
				append(args, result);
				append(args, node1(N_ADDR, id(ENDLF)));
				result = node2(N_CALL, id(MANIP), args, top);
				continue;
			}
			DataDef *vdd = vals[i]->datadef();
			ExternParam vp;
			const char *sym = ostream_insert_symbol(vdd, vp);
			need_output_extern(sym, true, { { {N_VOID}, true }, vp });
			node_t v = translate_expr(vals[i]);
			node_t args = list();
			append(args, result);
			append(args, v);
			result = node2(N_CALL, id(sym), args, top);
		}
```
(This replaces the Task-3 output branch; the `continue` skips to the next chain value after emitting the manipulator. Declaring `ENDLF` as a function returning void* and taking its address yields a function pointer arg — c2mir accepts `&fn`.)

- [ ] **Step 4: Run, expect PASS**

Run: `make -C src test 2>&1 | grep -iE "cout endl|ostream chains|FAILED" | head`
Expected: `"5\n"`, prior tests still green.

- [ ] **Step 5: Full suite + triage**

Run: `make -C src fulltest 2>&1 | tail -3` (419/0)
Run: `bash tmp/cir_triage.sh 2>/dev/null | sed -n '1,8p'` (record)

- [ ] **Step 6: Commit**

```bash
git add src/cir_builder.h src/cir_builder.cpp tests/unit/test_cir.cpp
git commit -m "feat(cir): cout/cerr endl manipulator via mangled std::endl

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Part B — `cin >>` input

**Files:** `src/cir_builder.h`, `src/cir_builder.cpp`, `tests/unit/test_cir.cpp`.

- [ ] **Step 1: Write the failing test** (feed stdin via a temp file the harness reads is awkward; instead test the emitted tree/runtime with a piped value using the existing `cir_capture` is not enough for input. Use a runtime test that reads from a string redirected onto fd 0.)

Add a helper test using `cir_capture` plus stdin redirection. Simplest: write a small input file and `freopen` stdin inside the test. Add this test:
```cpp
TEST_CASE("CirBuilder: cin >> int") {
    // Redirect stdin from a temp file containing "7", run, expect echo "7".
    FILE *f = fopen("/tmp/cir_cin_in.txt", "w"); fputs("7\n", f); fclose(f);
    FILE *old = stdin;
    stdin = fopen("/tmp/cir_cin_in.txt", "r");
    std::string out = cir_capture("int main() { int x; cin >> x; cout << x; return 0; }");
    fclose(stdin); stdin = old;
    CHECK(out == "7");
}
```
(If reassigning `stdin` is not portable in this libc, use `freopen("/tmp/cir_cin_in.txt","r",stdin)` then `freopen("/dev/tty","r",stdin)` to restore — pick whichever compiles. The key behavior: `cin >> x` reads 7, `cout << x` echoes it.)

- [ ] **Step 2: Run, expect FAIL** (`cin >>` becomes `N_RSH`)

Run: `make -C src test 2>&1 | grep -iE "cin >> int|FAILED" | head`

- [ ] **Step 3: Add the istream selector** in `src/cir_builder.cpp`

```cpp
const char *CirBuilder::istream_extract_symbol(DataDef *dd, ExternParam &p_out)
{
	DataType dt = dd ? dd->rawtype() : DataType::dtINT64;
	// operand is always a pointer to the target var; declare param as void* (ptr).
	p_out = {{N_VOID}, true};
	if (dd && dd->is_string())
		return "_ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE";
	switch (dt) {
	case DataType::dtCHAR:   return "_ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_RS3_";
	case DataType::dtDOUBLE:
	case DataType::dtFLOAT:  return "_ZNSirsERd";
	case DataType::dtINT16:
	case DataType::dtINT32:  return "_ZNSirsERi";
	case DataType::dtUINT8:
	case DataType::dtUINT16:
	case DataType::dtUINT32: return "_ZNSirsERj";
	default:                 return "_ZNSirsERl";
	}
}
```
Declare in header: `const char *istream_extract_symbol(DataDef *dd, ExternParam &p_out);`

- [ ] **Step 4: Add the input branch in `translate_stream_chain`** (the `else` of `if (is_out)`)

```cpp
		else {
			DataDef *vdd = vals[i]->datadef();
			ExternParam vp;
			const char *sym = istream_extract_symbol(vdd, vp);
			need_output_extern(sym, true, { { {N_VOID}, true }, vp });
			// operand is the ADDRESS of the target lvalue
			node_t target = translate_expr(vals[i]);
			node_t args = list();
			append(args, result);
			append(args, node1(N_ADDR, target));
			result = node2(N_CALL, id(sym), args, top);
		}
```

- [ ] **Step 5: Run, expect PASS**

Run: `make -C src test 2>&1 | grep -iE "cin >> int|FAILED" | head`
Expected: `"7"`.

- [ ] **Step 6: Demangle check + full suite + triage**

Run: `echo _ZNSirsERi | c++filt`  → Expected `std::istream::operator>>(int&)`.
Run: `make -C src fulltest 2>&1 | tail -3` (419/0)
Run: `bash tmp/cir_triage.sh 2>/dev/null | sed -n '1,8p'`

- [ ] **Step 7: Commit**

```bash
git add src/cir_builder.h src/cir_builder.cpp tests/unit/test_cir.cpp
git commit -m "feat(cir): cin >> input via mangled std::istream::operator>>

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: Measure, document, push

**Files:** memory `project_cir_triage.md`, `MEMORY.md`; KG.

- [ ] **Step 1: Final measurement**

Run: `bash scripts/run_tests.sh --backend=cir 2>/dev/null | tail -1` (record new pass count vs 171 baseline)
Run: `bash tmp/cir_triage.sh 2>/dev/null` (record full histogram + the new dominant `c2mir_rejected` causes for Phase-3)

- [ ] **Step 2: Update memory `project_cir_triage.md`** — append a "Phase-2 result" section: 171 → new count; per-part deltas (builtins, ostream, endl, cin); the remaining `c2mir_rejected` causes (type-coercion / struct-member / init) as the Phase-3 target. Update the `MEMORY.md` pointer line if the headline number changed.

- [ ] **Step 3: Update the KG** (if reachable): `scripts/kg_query.sh` — add a Session node `2026-05-29-cir-phase2-stream-io` summarizing the result. If unreachable, note sync debt.

- [ ] **Step 4: Commit any memory/doc changes that live in the repo, then push**

```bash
git push
```

---

## Self-review notes

- **Spec coverage:** Part A builtins (Task 1), Part B PoC (Task 2), full ostream table + chains + cerr/clog (Task 3), endl (Task 4), cin input (Task 5), measure/document (Task 6), output-capture harness (Task 0). All spec sections map to tasks.
- **No `c2m -d` oracle for Part B** (C has no streams) — verification is runtime output (`cir_capture`) + `c++filt` demangle confirmation, exactly as the spec states.
- **Type consistency:** `ExternParam {specs, ptr}`, `need_output_extern(symbol, ret_ptr, params)`, `need_stream_object(StreamKind)`, `stream_ident_kind`/`stream_object_symbol`/`translate_stream_chain`/`ostream_insert_symbol`/`istream_extract_symbol` names are used consistently across header and definitions and call sites.
- **Honesty gate:** every code task re-runs `tmp/cir_triage.sh` and records actuals incl. newly-surfaced modes; `fulltest` 419/0 gate on each.
- **Deferred (documented):** stream-typed variables, manipulators beyond endl, std::string-via-cout if CIR's string-object support isn't ready in output position (string literals use the const-char* overload and are unaffected) — note in the relevant commit if hit.
- **Risk:** the `m_output_externs`/`m_stream_object_protos` members must be declared in the header and appended in Pass 0.8; the stream-detection hook must sit BEFORE the `tkBSL`→`N_LSH` translation (Task 2 Step 7). Both called out in-task.
