/* test_cir.cpp — Unit tests for CIR translation (TokenBase → c2mir node_t → MIR) */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <dlfcn.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <iostream>
#include <sstream>
#include <fstream>
#include <stdint.h>
#include <asmjit/x86.h>

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

extern "C" {
#include "mir.h"
#include "mir-gen.h"
#include "c2mir/c2mir.h"
#include "c2mir/c2mir_api.h"
}

#include "../src/madc_cir.h"
#include "../src/cir_builder.h"

// The CIR output builtins (puti/printstr/...) lower to madc_* runtime symbols
// in madc_mir_backend.cpp.  MIR resolves them at link time via
// dlsym(RTLD_DEFAULT), which only sees symbols present in this binary's
// dynamic symbol table.  Since libmadc.a is linked as a plain archive, the
// backend object is only pulled in if something references it.  Take the
// address of each runtime symbol here (under -rdynamic) so the archive member
// is linked and the symbols are dlsym-visible during cir_capture().
extern "C" {
	void madc_puti(int64_t);
	void madc_putu(uint64_t);
	void madc_putd(double);
	void madc_putf(float);
	void madc_puts(const char *);
	void madc_printstr(const char *);
	bool cir_test_bool_true(void);
	bool cir_test_bool_false(void);
}
static void *const cir_runtime_anchor[] = {
	(void *)madc_puti, (void *)madc_putu, (void *)madc_putd,
	(void *)madc_putf, (void *)madc_puts, (void *)madc_printstr,
	(void *)cir_test_bool_true, (void *)cir_test_bool_false,
};

// Import resolver for MIR_link: external symbols (madc_* runtime, libc) are
// resolved via dlsym(RTLD_DEFAULT), mirroring madc_import_resolver in
// madc_mir_backend.cpp. Needed once a translated program calls a runtime
// symbol (e.g. the puti/printstr builtins -> madc_puti/madc_printstr).
static void *cir_test_import_resolver(const char *name) {
	return dlsym(RTLD_DEFAULT, name);
}

extern "C" bool cir_test_bool_true(void) {
	return true;
}

extern "C" bool cir_test_bool_false(void) {
	return false;
}

static size_t cir_count_tree1_copies(node_t n);

// Helper: translate an already-parsed Program to CIR, compile+run, return result.
static int64_t cir_run_program(Program *prog, size_t *tree1_copies = NULL) {
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);

    // Build the tree with the LIVE backend (CirBuilder::translate_module) —
    // exactly what bin/madc uses. The legacy static cir_translate() is retained
    // only for MADC_CIR_OLD=1 A/B comparison and DIVERGES from the live backend
    // (e.g. it mislowers backward `goto` loops into an unconditional infinite
    // loop, which hung this harness under MIR_interp). The builder owns its node
    // arena and must outlive cir_compile(), so it stays in scope until return.
    CirBuilder builder(c2m);
    node_t tree = builder.translate_module(prog);
    REQUIRE(tree != nullptr);
    if (tree1_copies)
	*tree1_copies = cir_count_tree1_copies(tree);

    int ok = cir_compile(mir_ctx, c2m, tree, "test_mod");
    REQUIRE(ok == 1);

    MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(mir_ctx));
    MIR_load_module(mir_ctx, mod);
    MIR_link(mir_ctx, MIR_set_interp_interface, cir_test_import_resolver);

    MIR_item_t func_item = NULL;
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
	 item != NULL;
	 item = DLIST_NEXT(MIR_item_t, item)) {
	if (item->item_type == MIR_func_item) {
	    if (strcmp(item->u.func->name, "main") == 0) {
		func_item = item;
	    }
	}
    }
    if (!func_item) {
	// Debug: dump what's in the module
	MIR_output_module(mir_ctx, stderr, mod);
    }
    REQUIRE(func_item != nullptr);

    MIR_val_t val;
    MIR_interp(mir_ctx, func_item, &val, 0);
    int64_t result = val.i;

    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);
    return result;
}

// Helper: tokenize+parse source, translate to CIR, compile+run, return result
static int64_t cir_run(const char *source) {
    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    return cir_run_program(prog.get());
}

static size_t cir_count_tree1_copies(node_t n) {
    if (!n)
	return 0;
    size_t count = CIR_NODE(n)->tree1_origin ? 1 : 0;
    for (node_t c = c2mir_node_first_op(n); c != NULL; c = c2mir_node_next_op(c))
	count += cir_count_tree1_copies(c);
    return count;
}

static bool parse_accepts_with_std(Program::LanguageStd language_std,
				   const char *source) {
    auto prog = std::make_shared<Program>();
    std::ostringstream err;
    prog->error_stream = &err;
    prog->language_std = language_std;
    TokenProgram *tp = prog->tokenize_buffer(source, "<std-mode-test>");
    if (!tp)
	return false;
    return prog->parse(tp);
}

TEST_CASE("auto-includes are limited to madc mode") {
    const char *source =
	"int main() { intptr_t n; n = 7; return (int)n; }";

    CHECK(parse_accepts_with_std(Program::STD_MADC, source));
    CHECK_FALSE(parse_accepts_with_std(Program::STD_C11, source));
    CHECK_FALSE(parse_accepts_with_std(Program::STD_CPP11, source));
}

TEST_CASE("explicit includes work in standard C and C++ modes") {
    const char *source =
	"#include <stdint.h>\n"
	"int main() { intptr_t n; n = 7; return (int)n; }";

    CHECK(parse_accepts_with_std(Program::STD_C11, source));
    CHECK(parse_accepts_with_std(Program::STD_CPP11, source));
}

TEST_CASE("shebang std option disables madc auto-includes") {
    CHECK_FALSE(parse_accepts_with_std(Program::STD_MADC,
	"#!/usr/bin/madc --std=c++\n"
	"int main() { intptr_t n; n = 7; return (int)n; }"));
}

TEST_CASE("CIR: return literal") {
    CHECK(cir_run("int main() { return 42; }") == 42);
    CHECK(cir_run("int main() { return 0; }") == 0);
    CHECK(cir_run("int main() { return -1; }") == -1);
}

TEST_CASE("CIR: arithmetic expressions") {
    CHECK(cir_run("int main() { return 2 + 3; }") == 5);
    CHECK(cir_run("int main() { return 10 - 3; }") == 7);
    CHECK(cir_run("int main() { return 6 * 7; }") == 42);
    CHECK(cir_run("int main() { return 100 / 10; }") == 10);
    CHECK(cir_run("int main() { return 17 % 5; }") == 2);
    CHECK(cir_run("int main() { return 2 + 3 * 4; }") == 14);
}

TEST_CASE("CIR: local variables") {
    CHECK(cir_run("int main() { int x; x = 10; return x; }") == 10);
    CHECK(cir_run("int main() { int x; x = 10; return x + 1; }") == 11);
    CHECK(cir_run("int main() { int a; int b; a = 3; b = 4; return a + b; }") == 7);
}

TEST_CASE("CIR: function with parameters") {
    CHECK(cir_run(
	"int add(int a, int b) { return a + b; }\n"
	"int main() { return add(3, 4); }"
    ) == 7);
}

TEST_CASE("CIR: external bool returns") {
    CHECK(cir_run(
	"bool cir_test_bool_true();\n"
	"bool cir_test_bool_false();\n"
	"int main() { return cir_test_bool_true() && !cir_test_bool_false(); }"
    ) == 1);
}

TEST_CASE("CIR: if/else") {
    CHECK(cir_run(
	"int main() { int x; x = 10; if (x > 5) return 1; return 0; }"
    ) == 1);
    CHECK(cir_run(
	"int main() { int x; x = 3; if (x > 5) return 1; else return 0; }"
    ) == 0);
}

TEST_CASE("CIR: while loop") {
    CHECK(cir_run(
	"int main() { int i; int sum; i = 0; sum = 0;\n"
	"  while (i < 10) { sum = sum + i; i = i + 1; }\n"
	"  return sum; }"
    ) == 45);
}

TEST_CASE("CIR: for loop") {
    CHECK(cir_run(
	"int main() { int sum; int i; sum = 0;\n"
	"  for (i = 1; i <= 10; i = i + 1) sum = sum + i;\n"
	"  return sum; }"
    ) == 55);
}

TEST_CASE("CIR: compound assignment") {
    CHECK(cir_run("int main() { int x; x = 10; x += 5; return x; }") == 15);
    CHECK(cir_run("int main() { int x; x = 10; x -= 3; return x; }") == 7);
    CHECK(cir_run("int main() { int x; x = 6; x *= 7; return x; }") == 42);
    CHECK(cir_run("int main() { int x; x = 100; x /= 10; return x; }") == 10);
    CHECK(cir_run("int main() { int x; x = 17; x %= 5; return x; }") == 2);
    CHECK(cir_run("int main() { int x; x = 0xFF; x &= 0x0F; return x; }") == 15);
    CHECK(cir_run("int main() { int x; x = 0xF0; x |= 0x0F; return x; }") == 255);
    CHECK(cir_run("int main() { int x; x = 1; x <<= 3; return x; }") == 8);
    CHECK(cir_run("int main() { int x; x = 16; x >>= 2; return x; }") == 4);
}

TEST_CASE("CIR: increment and decrement") {
    CHECK(cir_run("int main() { int x; x = 5; x++; return x; }") == 6);
    CHECK(cir_run("int main() { int x; x = 5; x--; return x; }") == 4);
    CHECK(cir_run("int main() { int x; x = 5; return ++x; }") == 6);
    CHECK(cir_run("int main() { int x; x = 5; return --x; }") == 4);
}

TEST_CASE("CIR: pointers") {
    CHECK(cir_run(
	"int main() { int x; int *p; x = 42; p = &x; return *p; }"
    ) == 42);
    CHECK(cir_run(
	"int main() { int x; int *p; x = 10; p = &x; *p = 20; return x; }"
    ) == 20);
}

TEST_CASE("CIR: arrays") {
    CHECK(cir_run(
	"int main() { int arr[5]; arr[0] = 10; arr[1] = 20; return arr[0] + arr[1]; }"
    ) == 30);
    CHECK(cir_run(
	"int main() { int arr[3]; int i; for (i = 0; i < 3; i = i + 1) arr[i] = i * 10;\n"
	"  return arr[0] + arr[1] + arr[2]; }"
    ) == 30);
}

TEST_CASE("CIR: structs") {
    CHECK(cir_run(
	"struct Point { int x; int y; };\n"
	"int main() { struct Point p; p.x = 3; p.y = 4; return p.x + p.y; }"
    ) == 7);
}

TEST_CASE("CIR: global variables") {
    CHECK(cir_run(
	"int g; int main() { g = 42; return g; }"
    ) == 42);
    CHECK(cir_run(
	"int counter;\n"
	"void inc(void) { counter = counter + 1; }\n"
	"int main() { counter = 0; inc(); inc(); inc(); return counter; }"
    ) == 3);
}

TEST_CASE("CIR: ternary operator") {
    CHECK(cir_run("int main() { int x; x = 10; return x > 5 ? 1 : 0; }") == 1);
    CHECK(cir_run("int main() { int x; x = 3; return x > 5 ? 1 : 0; }") == 0);
}

TEST_CASE("CIR: sizeof") {
    CHECK(cir_run("int main() { return sizeof(int); }") == 4);
    CHECK(cir_run("int main() { return sizeof(char); }") == 1);
    CHECK(cir_run("int main() { return sizeof(double); }") == 8);
}

TEST_CASE("CIR: cast") {
    CHECK(cir_run("int main() { double d; d = 3.7; return (int)d; }") == 3);
}

TEST_CASE("CIR: comma operator") {
    CHECK(cir_run("int main() { int x; x = (1, 2, 42); return x; }") == 42);
}

TEST_CASE("CIR: do-while") {
    CHECK(cir_run(
	"int main() { int i; int sum; i = 0; sum = 0;\n"
	"  do { sum = sum + i; i = i + 1; } while (i < 10);\n"
	"  return sum; }"
    ) == 45);
}

TEST_CASE("CIR: switch/case") {
    CHECK(cir_run(
	"int main() { int x; x = 2;\n"
	"  switch (x) { case 1: return 10; case 2: return 20; case 3: return 30; }\n"
	"  return 0; }"
    ) == 20);
}

TEST_CASE("CIR: break and continue") {
    CHECK(cir_run(
	"int main() { int i; int sum; sum = 0;\n"
	"  for (i = 0; i < 100; i = i + 1) {\n"
	"    if (i >= 5) break;\n"
	"    sum = sum + i;\n"
	"  }\n"
	"  return sum; }"
    ) == 10);
    CHECK(cir_run(
	"int main() { int i; int sum; sum = 0;\n"
	"  for (i = 0; i < 10; i = i + 1) {\n"
	"    if (i % 2 == 0) continue;\n"
	"    sum = sum + i;\n"
	"  }\n"
	"  return sum; }"
    ) == 25);
}

TEST_CASE("CIR: goto and labels") {
    CHECK(cir_run(
	"int main() { int x; x = 0; goto skip; x = 99; skip: return x; }"
    ) == 0);
    CHECK(cir_run(
	"int main() { int i; i = 0; loop: if (i >= 5) goto done;\n"
	"  i = i + 1; goto loop; done: return i; }"
    ) == 5);
}

TEST_CASE("CIR: variable initializers") {
    CHECK(cir_run("int main() { int x = 42; return x; }") == 42);
    CHECK(cir_run("int main() { int a = 3; int b = 4; return a + b; }") == 7);
}

TEST_CASE("CIR: enum constants") {
    CHECK(cir_run(
	"enum { RED, GREEN = 5, BLUE };\n"
	"int main() { return GREEN; }"
    ) == 5);
    CHECK(cir_run(
	"enum { A, B, C, D };\n"
	"int main() { return D; }"
    ) == 3);
}

TEST_CASE("CIR: static global") {
    CHECK(cir_run(
	"int count;\n"
	"void inc(void) { count = count + 1; }\n"
	"int main() { count = 0; inc(); inc(); inc(); return count; }"
    ) == 3);
}

TEST_CASE("CIR: nested function calls") {
    CHECK(cir_run(
	"int double_it(int x) { return x * 2; }\n"
	"int add_one(int x) { return x + 1; }\n"
	"int main() { return add_one(double_it(5)); }"
    ) == 11);
}

TEST_CASE("CIR: recursive function") {
    CHECK(cir_run(
	"int factorial(int n) { if (n <= 1) return 1; return n * factorial(n - 1); }\n"
	"int main() { return factorial(5); }"
    ) == 120);
}

TEST_CASE("CIR: static qualifier") {
    // static global — just tests that static doesn't break compilation
    CHECK(cir_run(
	"static int count;\n"
	"void inc(void) { count = count + 1; }\n"
	"int main() { count = 0; inc(); inc(); return count; }"
    ) == 2);
}

TEST_CASE("CIR: array brace initializer") {
    CHECK(cir_run(
	"int main() { int arr[3] = {10, 20, 30}; return arr[1]; }"
    ) == 20);
}

TEST_CASE("CIR: pointer arithmetic") {
    // Array decays to pointer; pointer + offset dereference
    CHECK(cir_run(
	"int main() { int a[4] = {1, 2, 3, 4}; return a[2]; }"
    ) == 3);
    CHECK(cir_run(
	"int main() { int a[4] = {1, 2, 3, 4}; return *(a + 3); }"
    ) == 4);
}

TEST_CASE("CIR: void function") {
    CHECK(cir_run(
	"int result;\n"
	"void set_result(int v) { result = v; }\n"
	"int main() { set_result(42); return result; }"
    ) == 42);
}

TEST_CASE("CIR: logical operators short-circuit") {
    CHECK(cir_run(
	"int main() { int x; x = 0; if (1 || (x = 99)) {} return x; }"
    ) == 0);
    CHECK(cir_run(
	"int main() { int x; x = 0; if (0 && (x = 99)) {} return x; }"
    ) == 0);
}

TEST_CASE("CIR: negative numbers and expressions") {
    CHECK(cir_run("int main() { return -42; }") == -42);
    CHECK(cir_run("int main() { int x; x = -10; return x * -3; }") == 30);
}

TEST_CASE("CIR: chained assignments") {
    CHECK(cir_run(
	"int main() { int a; int b; int c; a = b = c = 7; return a + b + c; }"
    ) == 21);
}

// --- CirBuilder tests (cir_node-based tree building) ---

// Helper: tokenize+parse, build tree with CirBuilder, compile+run, return result
static int64_t cir_run_builder(const char *source) {
    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));

    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);

    struct c2mir_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.message_file = stderr;
    c2m_ctx_t c2m = c2mir_init_compile(mir_ctx, &opts);
    REQUIRE(c2m != nullptr);

    CirBuilder builder(c2m);
    node_t tree = builder.translate_module(prog.get());
    REQUIRE(tree != nullptr);
    REQUIRE(cir_report_errors(stderr, tree) == 0);

    int ok = c2mir_compile_tree(mir_ctx, c2m, tree, "test_mod");
    REQUIRE(ok == 1);

    MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(mir_ctx));
    MIR_load_module(mir_ctx, mod);
    MIR_link(mir_ctx, MIR_set_interp_interface, cir_test_import_resolver);

    MIR_item_t func_item = NULL;
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
	 item != NULL;
	 item = DLIST_NEXT(MIR_item_t, item)) {
	if (item->item_type == MIR_func_item &&
	    strcmp(item->u.func->name, "main") == 0)
	    func_item = item;
    }
    REQUIRE(func_item != nullptr);

    MIR_val_t val;
    MIR_interp(mir_ctx, func_item, &val, 0);
    int64_t result = val.i;

    c2mir_finish_compile(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);
    return result;
}

// Like cir_run_builder, but captures everything the program writes to stdout
// (fd 1) during execution and returns it. Covers madc_puti/printf AND std::cout.
static std::string cir_capture(const char *source) {
    // Keep the runtime anchor (above) live so the backend object is linked.
    (void)cir_runtime_anchor;
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
    MIR_link(mir_ctx, MIR_set_interp_interface, cir_test_import_resolver);

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

static std::string cir_capture(const std::string &source) {
    return cir_capture(source.c_str());
}

TEST_CASE("CirBuilder: capture helper baseline") {
    // No output yet; just verify the helper runs a program and returns "".
    CHECK(cir_capture("int main() { return 0; }") == "");
}

TEST_CASE("CirBuilder: return literal") {
    CHECK(cir_run_builder("int main() { return 42; }") == 42);
    CHECK(cir_run_builder("int main() { return 0; }") == 0);
}

TEST_CASE("CirBuilder: char literals") {
    CHECK(cir_run_builder("int main() { return 'A'; }") == 65);
    CHECK(cir_run_builder("int main() { return 'Z' - 'A'; }") == 25);
}

TEST_CASE("CirBuilder: global initializers") {
    CHECK(cir_run_builder("int g = 5; int main() { return g; }") == 5);
    CHECK(cir_run_builder("int a = 3, b = 4; int main() { return a + b; }") == 7);
    CHECK(cir_run_builder(
        "int t[3] = {4,5,6}; int main() { return t[0]+t[1]+t[2]; }") == 15);
}

TEST_CASE("CirBuilder: arithmetic") {
    CHECK(cir_run_builder("int main() { return 2 + 3; }") == 5);
    CHECK(cir_run_builder("int main() { return 6 * 7; }") == 42);
}

TEST_CASE("CirBuilder: local vars and function call") {
    CHECK(cir_run_builder(
	"int add(int a, int b) { return a + b; }\n"
	"int main() { int x; x = add(10, 20); return x; }"
    ) == 30);
}

TEST_CASE("CirBuilder: stdout print builtins") {
    // madc_puti / puts append a newline.
    CHECK(cir_capture("int main() { puti(42); return 0; }") == "42\n");
    CHECK(cir_capture("#include <stdio.h>\nint main() { puts(\"hi\"); return 0; }") == "hi\n");
}

TEST_CASE("CirBuilder: if/else") {
    CHECK(cir_run_builder(
	"int main() { int x; x = 10; if (x > 5) return 1; else return 0; }"
    ) == 1);
}

TEST_CASE("CirBuilder: while loop") {
    CHECK(cir_run_builder(
	"int main() { int i; int sum; i = 0; sum = 0; while (i < 10) { sum = sum + i; i = i + 1; } return sum; }"
    ) == 45);
}

TEST_CASE("CirBuilder: scalar initializers") {
    CHECK(cir_run_builder("int main() { int x = 7; return x; }") == 7);
    CHECK(cir_run_builder("int main() { int a = 3, b = 4; return a + b; }") == 7);
}
TEST_CASE("CirBuilder: flat brace initializers") {
    CHECK(cir_run_builder(
        "int main() { int a[3] = {10,20,12}; return a[0]+a[1]+a[2]; }") == 42);
}
TEST_CASE("CirBuilder: structs") {
    CHECK(cir_run_builder(
	"struct point { int x; int y; };\n"
	"int main() { struct point p; p.x = 3; p.y = 4; return p.x + p.y; }"
    ) == 7);
}

TEST_CASE("CirBuilder: subscript-expr reads") {
    // base is an expression (pointer arithmetic), not a bare name -> TokenSubscriptExpr
    CHECK(cir_run_builder(
	"int main() { int a[3]; a[0]=7; a[1]=0; a[2]=0; int *p; p=a; return (p+0)[0]; }") == 7);
    // subscript-expr with a non-zero index offset on a sub-expression base
    CHECK(cir_run_builder(
	"int main() { int a[5]; a[0]=0; a[1]=0; a[2]=7; a[3]=0; a[4]=0; int *p; p=a; return (p+1)[1]; }") == 7);
    // Multi-dim fixed-array reads (a[i][j] via TokenSubscript::extra_indices) are
    // translated to nested N_IND, and are runtime-tested in the "nested brace
    // initializers" and "multi-dim array read" cases below (the decl lowering now
    // emits one N_ARR per dimension).
}

TEST_CASE("CirBuilder: nested brace initializers") {
    CHECK(cir_run_builder(
        "int main() { int m[2][2] = {{1,2},{3,4}}; return m[0][0] + m[1][1]; }") == 5);
    CHECK(cir_run_builder(
        "struct P { int x, y; };\n"
        "int main() { struct P a[2] = {{1,2},{3,4}}; return a[1].x + a[0].y; }") == 5);
}
TEST_CASE("CirBuilder: designated initializers") {
    // The madc parser normalizes designators into POSITIONAL slots at parse
    // time (parser.cpp: field name -> field_index, array [i] ->
    // assign_initializer_range), zero/NULL-filling gaps. CirBuilder therefore
    // never sees designator metadata and the existing positional INIT emission
    // is semantically correct without emitting N_FIELD_ID / index const-exprs.
    // field designators, out of order
    CHECK(cir_run_builder(
        "struct P { int x, y; };\n"
        "int main() { struct P p = { .y = 9, .x = 5 }; return p.x * 10 + p.y; }") == 59);
    // array index designators with gaps
    CHECK(cir_run_builder(
        "int main() { int d[5] = { [2] = 7, [4] = 1 }; return d[2]*10 + d[4] + d[0]; }") == 71);
}
TEST_CASE("CirBuilder: multi-dim array read") {
    CHECK(cir_run_builder(
        "int main() { int a[2][3]; a[1][2] = 8; return a[1][2]; }") == 8);
}

TEST_CASE("CirBuilder: cout << int (PoC)" * doctest::skip()) {
    CHECK(cir_capture("#include <iostream>\nusing namespace std;\n"
                      "int main() { cout << 5; return 0; }") == "5");
}

TEST_CASE("CirBuilder: ostream chains and types" * doctest::skip()) {
    const char *P = "#include <iostream>\nusing namespace std;\n";
    CHECK(cir_capture(std::string(P) + "int main() { cout << \"x=\" << 5 << '!'; return 0; }") == "x=5!");
    CHECK(cir_capture(std::string(P) + "int main() { double d = 1.5; cout << d; return 0; }") == "1.5");
    CHECK(cir_capture(std::string(P) + "int main() { cout << \"a\"; cerr << \"b\"; return 0; }") == "a");
}

TEST_CASE("CirBuilder: cout endl" * doctest::skip()) {
    const char *P = "#include <iostream>\nusing namespace std;\n";
    // endl as the trailing manipulator of a chain: the idiomatic form used by
    // essentially every cout integration test (`cout << ... << endl;`).
    CHECK(cir_capture(std::string(P) + "int main() { cout << 5 << endl; return 0; }") == "5\n");
    CHECK(cir_capture(std::string(P) + "int main() { cout << \"hi\" << endl; cout << 7; return 0; }") == "hi\n7");
    // LIMITATION: a value AFTER endl in the SAME chain (`cout << x << endl << y`)
    // is not supported on the CIR path. The legacy parser resolves std::endl as
    // the callable __std_endl, so when endl is followed by `<< y` it is parsed
    // as a call `endl()` and the chain is re-associated with cout dropped from
    // the root. Trailing endl (the idiomatic form) is unaffected. Fixing the
    // mid-chain form is a parser concern, not CIR lowering, and is out of scope.
}

TEST_CASE("CirBuilder: ostream parenthesized shift value" * doctest::skip()) {
    const char *P = "#include <iostream>\nusing namespace std;\n";
    // (1 << 2) is ONE value (a shift result), not two chain links.
    // madc parses << right-associatively, so 1 << 2 == 4 here; the point is
    // it prints a single value, not "12".
    CHECK(cir_capture(std::string(P) + "int main() { cout << (1 << 2); return 0; }") == "4");
    CHECK(cir_capture(std::string(P) + "int main() { cout << \"a\" << (1 << 2) << \"b\"; return 0; }") == "a4b");
}

TEST_CASE("CirJitSession resolves global data addresses by name") {
    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(
	"int counter = 4;\nint zeroed;\nlong big = 5000000000;\nint main() { return 0; }\n",
	"<test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));

    CirJitSession session;
    REQUIRE(session.build(prog.get(), "<test>"));
    void *addr = session.data_address("counter");
    REQUIRE(addr != (void *)NULL);
    CHECK(*(int *)addr == 4);
    void *big_addr = session.data_address("big");
    REQUIRE(big_addr != (void *)NULL);
    CHECK(*(int64_t *)big_addr == 5000000000LL);
    void *z = session.data_address("zeroed");      // zero-init: bss or data
    REQUIRE(z != (void *)NULL);
    CHECK(*(int *)z == 0);
    CHECK(session.data_address("no_such_global") == (void *)NULL);
}

// two-tree Phase 1.5: a stray, un-substituted template parameter must NOT
// silently mis-lower. append_type_specs turns it into an error node so the
// cir_report_errors gate rejects the tree (a Phase 2/3 substitution-bug guard).
TEST_CASE("CIR: unsubstituted template parameter lowers to an error node") {
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	DataDefTemplateParam tparam("T", 0);
	node_t lst = builder.type_list(&tparam);
	REQUIRE(lst != nullptr);
	// The placeholder reached lowering -> the spec list carries an error node.
	CHECK(cir_tree_has_error(lst));

	// A concrete type on the same path stays clean (the guard is specific).
	node_t ok = builder.type_list(&ddINT);
	REQUIRE(ok != nullptr);
	CHECK_FALSE(cir_tree_has_error(ok));
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);
}

// Two-tree Phase 2 / 2a: a dependent template-body parse resolves the template's
// own parameters to their placeholder through the EXISTING scoped type path. The
// registry is inert (empty stack) outside such a parse, so the common path is
// unchanged; here we drive it directly to prove the resolution + scoping.
TEST_CASE("CIR: scoped template-parameter registry resolves T to its placeholder") {
    auto prog = std::make_shared<Program>();

    // No active frame: a name is not a template parameter, and the scoped class
    // resolver (which now consults the registry first) still answers NULL.
    CHECK(prog->resolve_template_param("T") == nullptr);
    CHECK(prog->resolve_current_class_type_alias("T") == nullptr);

    {
	std::vector<std::string> params;
	params.push_back("T");
	params.push_back("U");
	Program::TemplateParamScope scope(*prog, params);

	DataDef *t = prog->resolve_template_param("T");
	DataDef *u = prog->resolve_template_param("U");
	REQUIRE(t != nullptr);
	REQUIRE(u != nullptr);
	CHECK(t->is_template_param());
	CHECK(u->is_template_param());
	CHECK(((DataDefTemplateParam *)t)->param_index == 0u);
	CHECK(((DataDefTemplateParam *)u)->param_index == 1u);

	// resolve_current_class_type_alias routes through the registry FIRST.
	CHECK(prog->resolve_current_class_type_alias("T") == t);
	// a non-parameter name still does not resolve as a type here.
	CHECK(prog->resolve_template_param("NotAParam") == nullptr);

	{
	    // a nested frame shadows the outer one for a repeated name, while the
	    // outer-only name stays visible through it.
	    std::vector<std::string> inner_params;
	    inner_params.push_back("T");
	    Program::TemplateParamScope inner(*prog, inner_params);
	    DataDef *t2 = prog->resolve_template_param("T");
	    REQUIRE(t2 != nullptr);
	    CHECK(t2->is_template_param());
	    CHECK(prog->resolve_template_param("U") == u);
	}

	// inner frame popped: T resolves to the outer placeholder again.
	CHECK(prog->resolve_template_param("T") == t);
    }

    // scope exited: no frame, nothing resolves.
    CHECK(prog->resolve_template_param("T") == nullptr);
    CHECK(prog->resolve_current_class_type_alias("T") == nullptr);
}

TEST_CASE("CIR: dependent pattern parse accepts template params in type positions") {
    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(
	"struct Holder {\n"
	"    int member;\n"
	"    template<class T> void cast_set(T v) { member = (int)(T)v + 1; }\n"
	"    template<class T> void local_set(T v) { T tmp = v; member = (int)tmp + 1; }\n"
	"};\n"
	"int main() { Holder h; h.cast_set(9); h.local_set(10); return h.member; }\n",
	"<dependent-pattern-test>");
    REQUIRE(tp != nullptr);

    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);
    bool ok = prog->parse(tp);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    REQUIRE(ok);

    size_t patterns = 0;
    for ( funcdef_map_iter it = prog->funcdef_map.begin();
	  it != prog->funcdef_map.end(); ++it )
	if ( it->second && it->second->dependent_pattern )
	    ++patterns;
    CHECK(patterns == 2);
}

// Two-tree Phase 4 (widening): a dependent return that is the bare param T wrapped in
// POINTER layers (`T echo(T v)`, `T* addr(T* p)`) is tsubst-eligible —
// build_dependent_pattern captures a recipe. A REFERENCE return (`T& ref(T& r)`) is NOT
// yet covered (a separate pre-existing ref-return-instance bug), so it is rejected; with
// echo + addr eligible and ref not, exactly two patterns are built.
TEST_CASE("CIR: bare-T and T* dependent returns are eligible, T& return is not") {
    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(
	"struct Holder {\n"
	"    int member;\n"
	"    template<class T> T echo(T v) { return v; }\n"
	"    template<class T> T* addr(T* p) { return p; }\n"
	"    template<class T> T& ref(T& r) { return r; }\n"
	"};\n"
	"int main() { Holder h; int x = 1; h.echo(7); h.addr(&x); h.ref(x); return h.member; }\n",
	"<dependent-return-test>");
    REQUIRE(tp != nullptr);

    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);
    bool ok = prog->parse(tp);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    REQUIRE(ok);

    size_t patterns = 0;
    bool echo_has_pattern = false, addr_has_pattern = false, ref_has_pattern = false;
    for ( funcdef_map_iter it = prog->funcdef_map.begin();
	  it != prog->funcdef_map.end(); ++it )
	if ( it->second && it->second->dependent_pattern ) {
	    ++patterns;
	    if ( it->first.find("echo") != std::string::npos )
		echo_has_pattern = true;
	    if ( it->first.find("addr") != std::string::npos )
		addr_has_pattern = true;
	    if ( it->first.find("ref") != std::string::npos )
		ref_has_pattern = true;
	}
    CHECK(patterns == 2);
    CHECK(echo_has_pattern);
    CHECK(addr_has_pattern);
    CHECK(!ref_has_pattern);
}

// Two-tree Phase 4 (widening): a member template with MULTIPLE type params
// (`template<class A, class B> void set2(A a, B b)`) is tsubst-eligible. A
// single-param template stays eligible too, so both build recipes (two patterns).
TEST_CASE("CIR: a multi-type-param member template is tsubst-eligible") {
    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(
	"struct Holder {\n"
	"    int member;\n"
	"    template<class A, class B> void set2(A a, B b) { member = (int)a + (int)b; }\n"
	"    template<class T> T echo(T v) { return v; }\n"
	"};\n"
	"int main() { Holder h; h.set2(3, 4); h.echo(7); return h.member; }\n",
	"<multi-param-test>");
    REQUIRE(tp != nullptr);

    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);
    bool ok = prog->parse(tp);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    REQUIRE(ok);

    size_t patterns = 0;
    bool set2_has_pattern = false, echo_has_pattern = false;
    for ( funcdef_map_iter it = prog->funcdef_map.begin();
	  it != prog->funcdef_map.end(); ++it )
	if ( it->second && it->second->dependent_pattern ) {
	    ++patterns;
	    if ( it->first.find("set2") != std::string::npos )
		set2_has_pattern = true;
	    if ( it->first.find("echo") != std::string::npos )
		echo_has_pattern = true;
	}
    CHECK(patterns == 2);
    CHECK(set2_has_pattern);
    CHECK(echo_has_pattern);
}

// Two-tree direct type-arg binding: the concrete instantiated FuncDef records the
// parser's deduced/explicit type arguments in template-param order. This covers a
// param that appears only inside the body; signature-based recovery cannot bind it.
TEST_CASE("CIR: direct tsubst args cover a body-only template parameter") {
    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(
	"struct Holder {\n"
	"    int member;\n"
	"    template<class T> void body_only(int x) { T tmp = (T)x; member = (int)tmp; }\n"
	"};\n"
	"int main() { Holder h; h.body_only<int>(7); return h.member; }\n",
	"<body-only-type-arg-test>");
    REQUIRE(tp != nullptr);

    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);
    bool ok = prog->parse(tp);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    REQUIRE(ok);

    FuncDef *source = NULL;
    FuncDef *instance = NULL;
    for ( funcdef_map_iter it = prog->funcdef_map.begin();
	  it != prog->funcdef_map.end(); ++it )
    {
	FuncDef *fd = it->second;
	if ( !fd )
	    continue;
	if ( fd->dependent_pattern
	  && it->first.find("body_only") != std::string::npos )
	    source = fd;
	if ( fd->tsubst_source
	  && fd->tsubst_source->method_display_name == "body_only" )
	    instance = fd;
    }
    REQUIRE(source != NULL);
    REQUIRE(instance != NULL);
    CHECK(instance->tsubst_source == source);
    REQUIRE(instance->tsubst_type_args.size() == 1);
    REQUIRE(instance->tsubst_type_args[0] != NULL);
    CHECK(instance->tsubst_type_args[0]->rawtype() == DataType::dtINT32);
}

// Two-tree pack prerequisite: the parser already deduces concrete pack element
// types for the re-parse path. Preserve those as durable instance metadata so
// CIR pack expansion can later fan out the Tree-1 pattern by real arity/types.
TEST_CASE("CIR: direct tsubst args record member-template type packs") {
    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(
	"struct Holder {\n"
	"    int member;\n"
	"    template<class... Args> void pack_body(Args... args) { member = 3; }\n"
	"};\n"
	"int main() { Holder h; h.pack_body(1, 2); return h.member; }\n",
	"<pack-type-arg-test>");
    REQUIRE(tp != nullptr);

    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);
    bool ok = prog->parse(tp);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    REQUIRE(ok);

    FuncDef *source = NULL;
    FuncDef *instance = NULL;
    for ( funcdef_map_iter it = prog->funcdef_map.begin();
	  it != prog->funcdef_map.end(); ++it )
    {
	FuncDef *fd = it->second;
	if ( !fd )
	    continue;
	if ( fd->is_member_template
	  && fd->method_display_name == "pack_body" )
	    source = fd;
	if ( fd->tsubst_source
	  && fd->tsubst_source->method_display_name == "pack_body" )
	    instance = fd;
    }
    REQUIRE(source != NULL);
    REQUIRE(instance != NULL);
    CHECK(instance->tsubst_source == source);
    CHECK(instance->tsubst_type_args.empty());
    REQUIRE(instance->tsubst_type_arg_packs.size() == 1);
    REQUIRE(instance->tsubst_type_arg_packs[0].size() == 2);
    REQUIRE(instance->tsubst_type_arg_packs[0][0] != NULL);
    REQUIRE(instance->tsubst_type_arg_packs[0][1] != NULL);
    CHECK(instance->tsubst_type_arg_packs[0][0]->rawtype() == DataType::dtINT32);
    CHECK(instance->tsubst_type_arg_packs[0][1]->rawtype() == DataType::dtINT32);
}

// Two-tree pack expansion: a saved Tree-1 body containing a direct value-pack
// expansion in a call argument list (`sink(args...)`) is copied by CIR tsubst
// into one argument node per concrete pack element.
TEST_CASE("CIR: tsubst fans out direct value-pack call arguments") {
    const char *source =
	"int sink(int a, int b) { return a * 10 + b; }\n"
	"struct Holder {\n"
	"    template<class... Args> int pack_call(Args... args) { return sink(args...); }\n"
	"};\n"
	"int main() { Holder h; return h.pack_call(3, 4); }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<pack-fanout-test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 34);
}

// Two-tree pack expansion: the expansion marker can sit on a larger expression
// tree, not only on a bare pack id. The copied expression must still rename the
// value-pack leaves per concrete element.
TEST_CASE("CIR: tsubst fans out expression-pack call arguments") {
    const char *source =
	"int sink(int a, int b) { return a * 10 + b; }\n"
	"struct Holder {\n"
	"    template<class... Args> int pack_expr(Args... args) { return sink((args + 1)...); }\n"
	"};\n"
	"int main() { Holder h; return h.pack_expr(3, 4); }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<pack-expr-fanout-test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 45);
}

// Two-tree pack expansion: a pack-expanded dependent call such as
// `std::forward<Args>(args)...` must clone the call once per concrete pack
// element, re-resolve the copied callee id to the concrete specialization, and
// rename the value-pack argument leaves.
TEST_CASE("CIR: tsubst fans out forwarding call-pack arguments") {
    const char *source =
	"int sink(int a, int b) { return a * 10 + b; }\n"
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Holder {\n"
	"    template<class... Args> int pack_forward(Args... args) { return sink(std::forward<Args>(args)...); }\n"
	"};\n"
	"int main() { Holder h; return h.pack_forward(3, 4); }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<pack-forward-fanout-test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 34);
}

// Two-tree dependent-call widening: a non-pack namespace function-template call
// nested inside an otherwise covered member-template body should be re-resolved
// from the substituted argument types instead of forcing re-parse. Explicit
// `ident<T>` template-id calls are a separate later body-surface slice.
TEST_CASE("CIR: tsubst re-resolves nested dependent namespace calls") {
    const char *source =
	"namespace nn {\n"
	"    template<class T> T ident(T v) { return v; }\n"
	"}\n"
	"int sink(int v) { return v + 5; }\n"
	"struct Holder {\n"
	"    template<class T> int nested(T v) { return sink(nn::ident(v)); }\n"
	"};\n"
	"int main() { Holder h; return h.nested(37); }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<nested-dependent-call-test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 42);
}

// System-header dependent calls stay guarded to simple scalar/pointer calls
// whose substituted argument and return types are concrete non-class shapes.
// This pins the reserved-helper slice without admitting broader object-address
// cases.
TEST_CASE("CIR: tsubst re-resolves scalar system-header dependent namespace calls") {
    const char *header_source =
	"namespace nn {\n"
	"    template<class T> T __ident(T v) { return v; }\n"
	"}\n"
	"struct Holder {\n"
	"    template<class T> int nested(T v) { return nn::__ident(v) + 5; }\n"
	"};\n";
    const char *main_source =
	"int main() { Holder h; return h.nested(37); }\n";
    const char *run_source =
	"namespace nn {\n"
	"    template<class T> T __ident(T v) { return v; }\n"
	"}\n"
	"struct Holder {\n"
	"    template<class T> int nested(T v) { return nn::__ident(v) + 5; }\n"
	"};\n"
	"int main() { Holder h; return h.nested(37); }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *hdr = prog->tokenize_buffer(
	header_source, "/usr/include/c++/13/bits/scalar_call.h");
    REQUIRE(hdr != nullptr);
    REQUIRE(prog->parse(hdr));
    TokenProgram *tp = prog->tokenize_buffer(main_source, "<system-header-nested-call-user>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    size_t tree1_copies = 0;
    int64_t got = cir_run_program(prog.get(), &tree1_copies);
    CHECK(tree1_copies > 0);
    CHECK(cir_run(run_source) == 42);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 42);
}

// Two-tree pack expansion: allocator-style placement construction uses the same
// forwarding-call pack, but as constructor arguments to `new ((void*)p) T(...)`
// rather than as ordinary function-call arguments.
TEST_CASE("CIR: tsubst fans out placement-new constructor pack arguments") {
    const char *source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Box {\n"
	"    int x;\n"
	"    Box(int a, int b) { x = a * 10 + b; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class... Args> void make(Box* p, Args... args) { new ((void*)p) Box(std::forward<Args>(args)...); }\n"
	"};\n"
	"int main() { Box b(0, 0); Maker m; m.make(&b, 3, 4); return b.x; }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<pack-new-fanout-test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 34);
}

// The same placement-new pack shape must not be excluded merely because the
// retained template body came from a system-header path; this is the
// `__new_allocator::construct` / `std::_Construct` shape.
TEST_CASE("CIR: tsubst admits system-header placement-new pack bodies") {
    const char *header_source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Box {\n"
	"    int x;\n"
	"    Box(int a, int b) { x = a * 10 + b; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class... Args> void make(Box* p, Args... args) { new ((void*)p) Box(std::forward<Args>(args)...); }\n"
	"};\n";
    const char *main_source =
	"int main() { Box b(0, 0); Maker m; m.make(&b, 3, 4); return b.x; }\n";
    const char *run_source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Box {\n"
	"    int x;\n"
	"    Box(int a, int b) { x = a * 10 + b; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class... Args> void make(Box* p, Args... args) { new ((void*)p) Box(std::forward<Args>(args)...); }\n"
	"};\n"
	"int main() { Box b(0, 0); Maker m; m.make(&b, 3, 4); return b.x; }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *hdr = prog->tokenize_buffer(
	header_source, "/usr/include/c++/13/bits/new_allocator.h");
    REQUIRE(hdr != nullptr);
    REQUIRE(prog->parse(hdr));
    TokenProgram *tp = prog->tokenize_buffer(main_source, "<pack-new-user>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(run_source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 34);
}

// Real allocator construction spells the constructed type itself as a template
// parameter (`_Up`). For scalar/pointer `_Up`, tsubst can lower after substituting
// the concrete type; a singleton pack in assignment position becomes that one
// constructor argument.
TEST_CASE("CIR: tsubst lowers system-header scalar placement-new template type") {
    const char *header_source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n";
    const char *main_source =
	"int main() { int x = 0; Maker m; m.make(&x, 42); return x; }\n";
    const char *run_source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n"
	"int main() { int x = 0; Maker m; m.make(&x, 42); return x; }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *hdr = prog->tokenize_buffer(
	header_source, "/usr/include/c++/13/bits/new_allocator.h");
    REQUIRE(hdr != nullptr);
    REQUIRE(prog->parse(hdr));
    TokenProgram *tp = prog->tokenize_buffer(main_source, "<pack-new-scalar-user>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(run_source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 42);
}

// Class `_Up` placement construction uses the same deferred constructed-type
// marker as scalar `_Up`, but after substitution it must run the existing class
// placement-new lowering instead of falling back to the re-parsed body.
TEST_CASE("CIR: tsubst lowers system-header class placement-new template type") {
    const char *header_source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Box {\n"
	"    int x;\n"
	"    Box(int a, int b) { x = a * 10 + b; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n";
    const char *main_source =
	"int main() { Box b(0, 0); Maker m; m.make(&b, 3, 4); return b.x; }\n";
    const char *run_source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Box {\n"
	"    int x;\n"
	"    Box(int a, int b) { x = a * 10 + b; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n"
	"int main() { Box b(0, 0); Maker m; m.make(&b, 3, 4); return b.x; }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *hdr = prog->tokenize_buffer(
	header_source, "/usr/include/c++/13/bits/new_allocator.h");
    REQUIRE(hdr != nullptr);
    REQUIRE(prog->parse(hdr));
    TokenProgram *tp = prog->tokenize_buffer(main_source, "<pack-new-class-user>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(run_source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 34);
}

// A singleton class object in the forwarded constructor pack must stay as the
// marked pack expression until copy-time substitution. Materializing it before
// tsubst loses the renamed parameter and leaks the temp declaration outside the
// instantiated body.
TEST_CASE("CIR: tsubst lowers singleton class-object placement-new packs") {
    const char *header_source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Item {\n"
	"    int x;\n"
	"    Item(int v) { x = v; }\n"
	"};\n"
	"struct Box {\n"
	"    int x;\n"
	"    Box(Item i) { x = i.x + 5; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n";
    const char *main_source =
	"int main() { Item zero(0); Box b(zero); Maker m; Item it(37); m.make(&b, it); return b.x; }\n";
    const char *run_source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Item {\n"
	"    int x;\n"
	"    Item(int v) { x = v; }\n"
	"};\n"
	"struct Box {\n"
	"    int x;\n"
	"    Box(Item i) { x = i.x + 5; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n"
	"int main() { Item zero(0); Box b(zero); Maker m; Item it(37); m.make(&b, it); return b.x; }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *hdr = prog->tokenize_buffer(
	header_source, "/usr/include/c++/13/bits/new_allocator.h");
    REQUIRE(hdr != nullptr);
    REQUIRE(prog->parse(hdr));
    TokenProgram *tp = prog->tokenize_buffer(main_source, "<pack-new-class-object-user>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(run_source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 42);
}

// Multi-element by-value class-object packs are the same lowering class as the
// singleton case, but each expanded element must be checked against its own
// constructor parameter before the pack can stay on the Tree-1 path.
TEST_CASE("CIR: tsubst lowers multi-element class-object placement-new packs") {
    const char *header_source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Item {\n"
	"    int x;\n"
	"    Item(int v) { x = v; }\n"
	"};\n"
	"struct PairBox {\n"
	"    int x;\n"
	"    PairBox(Item a, Item b) { x = a.x * 10 + b.x; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n";
    const char *main_source =
	"int main() { Item zero(0); PairBox box(zero, zero); Maker m; Item a(3); Item b(4); m.make(&box, a, b); return box.x; }\n";
    const char *run_source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Item {\n"
	"    int x;\n"
	"    Item(int v) { x = v; }\n"
	"};\n"
	"struct PairBox {\n"
	"    int x;\n"
	"    PairBox(Item a, Item b) { x = a.x * 10 + b.x; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n"
	"int main() { Item zero(0); PairBox box(zero, zero); Maker m; Item a(3); Item b(4); m.make(&box, a, b); return box.x; }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *hdr = prog->tokenize_buffer(
	header_source, "/usr/include/c++/13/bits/new_allocator.h");
    REQUIRE(hdr != nullptr);
    REQUIRE(prog->parse(hdr));
    TokenProgram *tp = prog->tokenize_buffer(main_source, "<pack-new-multi-class-object-user>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(run_source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 34);
}

// Value-returning forwarded class objects bound to reference constructor
// parameters need object-address lowering per expanded pack element; the temp
// materialization must stay inside the copied placement expression, not leak
// into the caller that instantiated the template.
TEST_CASE("CIR: tsubst lowers value-returning class-reference placement-new packs") {
    const char *header_source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Item {\n"
	"    int x;\n"
	"    Item(int v) { x = v; }\n"
	"};\n"
	"struct PairRef {\n"
	"    int x;\n"
	"    PairRef(const Item& a, const Item& b) { x = a.x * 10 + b.x; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n";
    const char *main_source =
	"int main() { Item zero(0); PairRef box(zero, zero); Maker m; Item a(3); Item b(4); m.make(&box, a, b); return box.x; }\n";
    const char *run_source =
	"namespace std {\n"
	"    template<class T> T forward(T v) { return v; }\n"
	"}\n"
	"struct Item {\n"
	"    int x;\n"
	"    Item(int v) { x = v; }\n"
	"};\n"
	"struct PairRef {\n"
	"    int x;\n"
	"    PairRef(const Item& a, const Item& b) { x = a.x * 10 + b.x; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n"
	"int main() { Item zero(0); PairRef box(zero, zero); Maker m; Item a(3); Item b(4); m.make(&box, a, b); return box.x; }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *hdr = prog->tokenize_buffer(
	header_source, "/usr/include/c++/13/bits/new_allocator.h");
    REQUIRE(hdr != nullptr);
    REQUIRE(prog->parse(hdr));
    TokenProgram *tp = prog->tokenize_buffer(main_source, "<pack-new-class-ref-user>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(run_source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 34);
}

// Reference-returning forward packs are addressable class objects. For local
// retained recipes, the copied placement-new expression can re-take each
// substituted element's address instead of falling back to re-parse. Real
// system-header object-address packs are still broader Phase 4 work.
TEST_CASE("CIR: tsubst lowers reference-forwarded class-reference placement-new packs") {
    const char *header_source =
	"namespace std {\n"
	"    template<class T> T& forward(T& v) { return v; }\n"
	"}\n"
	"struct Item {\n"
	"    int x;\n"
	"    Item(int v) { x = v; }\n"
	"};\n"
	"struct PairRef {\n"
	"    int x;\n"
	"    PairRef(const Item& a, const Item& b) { x = a.x * 10 + b.x; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args&... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n";
    const char *main_source =
	"int main() { Item zero(0); PairRef box(zero, zero); Maker m; Item a(3); Item b(4); m.make(&box, a, b); return box.x; }\n";
    const char *run_source =
	"namespace std {\n"
	"    template<class T> T& forward(T& v) { return v; }\n"
	"}\n"
	"struct Item {\n"
	"    int x;\n"
	"    Item(int v) { x = v; }\n"
	"};\n"
	"struct PairRef {\n"
	"    int x;\n"
	"    PairRef(const Item& a, const Item& b) { x = a.x * 10 + b.x; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args&... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n"
	"int main() { Item zero(0); PairRef box(zero, zero); Maker m; Item a(3); Item b(4); m.make(&box, a, b); return box.x; }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *hdr = prog->tokenize_buffer(
	header_source, "<pack-new-ref-forward-class-ref-header>");
    REQUIRE(hdr != nullptr);
    REQUIRE(prog->parse(hdr));
    TokenProgram *tp = prog->tokenize_buffer(main_source, "<pack-new-ref-forward-class-ref-user>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(run_source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 34);
}

// The same direct forwarding-call pack is safe from a real system-header path
// only when each expanded element can re-enter copied dependent-call resolution.
// This pins the guarded aperture without admitting arbitrary object-address
// pack expressions.
TEST_CASE("CIR: tsubst lowers system-header reference-forwarded placement-new packs") {
    const char *header_source =
	"namespace std {\n"
	"    template<class T> T& forward(T& v) { return v; }\n"
	"}\n"
	"struct Item {\n"
	"    int x;\n"
	"    Item(int v) { x = v; }\n"
	"};\n"
	"struct PairRef {\n"
	"    int x;\n"
	"    PairRef(const Item& a, const Item& b) { x = a.x * 10 + b.x; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args&... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n";
    const char *main_source =
	"int main() { Item zero(0); PairRef box(zero, zero); Maker m; Item a(3); Item b(4); m.make(&box, a, b); return box.x; }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *hdr = prog->tokenize_buffer(
	header_source, "/usr/include/c++/13/bits/new_allocator.h");
    REQUIRE(hdr != nullptr);
    REQUIRE(prog->parse(hdr));
    TokenProgram *tp = prog->tokenize_buffer(
	main_source, "<pack-new-system-ref-forward-class-ref-user>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    size_t tree1_copies = 0;
    int64_t got = cir_run_program(prog.get(), &tree1_copies);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(tree1_copies > 0);
    CHECK(got == 34);
}

// A system-header forwarding pack may return a different class than the
// constructor reference parameter expects when the target class has a
// converting constructor. This keeps the guarded copied-call requirement, but
// materializes the converted target temporary before the placement-new ctor.
TEST_CASE("CIR: tsubst lowers system-header converted reference-forwarded placement-new packs") {
    const char *header_source =
	"namespace std {\n"
	"    template<class T> T& forward(T& v) { return v; }\n"
	"}\n"
	"struct Item {\n"
	"    int x;\n"
	"    Item(int v) { x = v; }\n"
	"};\n"
	"struct Wrap {\n"
	"    int x;\n"
	"    Wrap(const Item& i) { x = i.x + 10; }\n"
	"};\n"
	"struct PairWrap {\n"
	"    int x;\n"
	"    PairWrap(const Wrap& a, const Wrap& b) { x = a.x * 10 + b.x; }\n"
	"};\n"
	"struct Maker {\n"
	"    template<class Up, class... Args> void make(Up* p, Args&... args) { new ((void*)p) Up(std::forward<Args>(args)...); }\n"
	"};\n";
    const char *main_source =
	"int main() { Item zero(0); Wrap wz(zero); PairWrap box(wz, wz); Maker m; Item a(2); Item b(5); m.make(&box, a, b); return box.x; }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *hdr = prog->tokenize_buffer(
	header_source, "/usr/include/c++/13/bits/new_allocator.h");
    REQUIRE(hdr != nullptr);
    REQUIRE(prog->parse(hdr));
    TokenProgram *tp = prog->tokenize_buffer(
	main_source, "<pack-new-system-converted-ref-forward-user>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    size_t tree1_copies = 0;
    int64_t got = cir_run_program(prog.get(), &tree1_copies);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(tree1_copies > 0);
    CHECK(got == 135);
}

// `__destroy(T*)` must not be lowered while T is still a placeholder in the
// retained recipe; the concrete pointee class is known only after tsubst.
TEST_CASE("CIR: tsubst lowers direct dependent destroy helper") {
    const char *header_source =
	"struct Box {\n"
	"    int* p;\n"
	"    Box(int* q) { p = q; }\n"
	"    ~Box() { *p = *p + 7; }\n"
	"};\n"
	"struct Cleaner {\n"
	"    template<class T> void clean(T* p) { __destroy(p); }\n"
	"};\n";
    const char *main_source =
	"int main() { int x = 0; Box* b = new Box(&x); Cleaner c; c.clean(b); return x; }\n";
    const char *run_source =
	"struct Box {\n"
	"    int* p;\n"
	"    Box(int* q) { p = q; }\n"
	"    ~Box() { *p = *p + 7; }\n"
	"};\n"
	"struct Cleaner {\n"
	"    template<class T> void clean(T* p) { __destroy(p); }\n"
	"};\n"
	"int main() { int x = 0; Box* b = new Box(&x); Cleaner c; c.clean(b); return x; }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *hdr = prog->tokenize_buffer(
	header_source, "/usr/include/c++/13/bits/stl_construct.h");
    REQUIRE(hdr != nullptr);
    REQUIRE(prog->parse(hdr));
    TokenProgram *tp = prog->tokenize_buffer(main_source, "<destroy-helper-user>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    FuncDef *source = NULL;
    FuncDef *instance = NULL;
    for ( funcdef_map_iter it = prog->funcdef_map.begin();
	  it != prog->funcdef_map.end(); ++it )
    {
	FuncDef *fd = it->second;
	if ( !fd )
	    continue;
	if ( fd->dependent_pattern
	  && fd->method_display_name == "clean" )
	    source = fd;
	if ( fd->tsubst_source
	  && fd->tsubst_source->method_display_name == "clean" )
	    instance = fd;
    }
    REQUIRE(source != NULL);
    REQUIRE(instance != NULL);
    CHECK(instance->tsubst_source == source);
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(run_source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 7);
}

// A member template named `__destroy` is the _Destroy_aux guard shape. It is
// safe to copy only when the retained body contains direct compiler intrinsic
// destroy markers, so the concrete pointee class is known at tsubst time.
TEST_CASE("CIR: tsubst lowers direct destroy-aux member body") {
    const char *header_source =
	"struct Box {\n"
	"    int* p;\n"
	"    Box(int* q) { p = q; }\n"
	"    ~Box() { *p = *p + 7; }\n"
	"};\n"
	"struct DestroyAux {\n"
	"    template<class T> static void __destroy(T* p) { ::__destroy(p); }\n"
	"};\n"
	"";
    const char *main_source =
	"int main() { int x = 0; Box* b = new Box(&x); DestroyAux d; d.__destroy(b); return x; }\n";
    std::string run_source = std::string(header_source) + main_source;
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *hdr = prog->tokenize_buffer(
	header_source, "/usr/include/c++/13/bits/stl_construct.h");
    REQUIRE(hdr != nullptr);
    REQUIRE(prog->parse(hdr));
    TokenProgram *tp = prog->tokenize_buffer(main_source, "<destroy-aux-user>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    FuncDef *source_fd = NULL;
    FuncDef *instance = NULL;
    for ( funcdef_map_iter it = prog->funcdef_map.begin();
	  it != prog->funcdef_map.end(); ++it )
    {
	FuncDef *fd = it->second;
	if ( !fd )
	    continue;
	if ( fd->dependent_pattern
	  && fd->method_display_name == "__destroy" )
	    source_fd = fd;
	if ( fd->tsubst_source
	  && fd->tsubst_source->method_display_name == "__destroy" )
	    instance = fd;
    }
    REQUIRE(source_fd != NULL);
    REQUIRE(instance != NULL);
    CHECK(instance->tsubst_source == source_fd);
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(run_source.c_str());
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 7);
}

// Two-tree pack expansion: reference parameter packs have already lowered value
// reads through the reference pointer in the Tree-1 recipe. They can therefore
// use the same direct fan-out and args -> args__N rename as by-value packs.
TEST_CASE("CIR: tsubst fans out direct reference-pack call arguments") {
    const char *source =
	"int sink(int a, int b) { return a * 10 + b; }\n"
	"struct Holder {\n"
	"    template<class... Args> int pack_ref(Args&... args) { return sink(args...); }\n"
	"};\n"
	"int main() { Holder h; int a = 3; int b = 4; return h.pack_ref(a, b); }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<pack-ref-fanout-test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 34);
}

// Two-tree pack expansion: pointer parameter packs use the same declaration
// fan-out as references, but the declarator suffix must stay attached to every
// generated parameter (`Args*... ps` -> `T0* ps__0, T1* ps__1`).
TEST_CASE("CIR: tsubst fans out direct pointer-pack call arguments") {
    const char *source =
	"int sink(int *a, int *b) { return *a * 10 + *b; }\n"
	"struct Holder {\n"
	"    template<class... Args> int pack_ptr(Args*... ps) { return sink(ps...); }\n"
	"};\n"
	"int main() { Holder h; int a = 3; int b = 4; return h.pack_ptr(&a, &b); }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<pack-ptr-fanout-test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 34);
}

// Two-tree pack expansion: member-template constructors use the same retained
// body + concrete pack metadata model as member functions. A covered ctor body
// should therefore be copied from Tree-1 instead of only relying on re-parse.
TEST_CASE("CIR: tsubst fans out constructor value-pack call arguments") {
    const char *source =
	"int sink(int a, int b) { return a * 10 + b; }\n"
	"struct Holder {\n"
	"    int member;\n"
	"    template<class... Args> Holder(Args... args) { member = sink(args...); }\n"
	"};\n"
	"int main() { Holder h(3, 4); return h.member; }\n";
    const char *old_env = getenv("MADC_XTEST_DEP_PARSE");
    std::string saved_env = old_env ? old_env : "";
    setenv("MADC_XTEST_DEP_PARSE", "1", 1);

    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<pack-ctor-fanout-test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog.get());
	REQUIRE(tree != nullptr);
	CHECK(cir_count_tree1_copies(tree) > 0);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);

    int64_t got = cir_run(source);
    if ( old_env )
	setenv("MADC_XTEST_DEP_PARSE", saved_env.c_str(), 1);
    else
	unsetenv("MADC_XTEST_DEP_PARSE");
    CHECK(got == 34);
}

// Two-tree Phase 3: tsubst_cir = copy_cir_subtree + substitute template-parameter
// placeholders with concrete types. The saved pattern (Tree-1) is left untouched;
// the returned copy (Tree-2) carries the concrete type. Drives the scalar case
// (T -> int) directly. Derived-layer peeling (T*/T&/const T) widens later.
TEST_CASE("CIR: tsubst_cir substitutes a template-parameter placeholder") {
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);

	DataDefTemplateParam T("T", 0);

	// A tiny pattern: an interior node with one child id whose madc type is the
	// placeholder T.
	node_t leaf = builder.id("v");
	CIR_NODE(leaf)->datadef = &T;
	node_t pattern = builder.node1(N_ADDR, leaf);

	std::map<DataDef *, DataDef *> subst;
	subst[&T] = &ddINT;

	cir_node *copy = builder.tsubst_cir(CIR_NODE(pattern), subst);
	REQUIRE(copy != nullptr);

	// The copy is a fresh node distinct from the source, back-linked to Tree-1.
	CHECK(copy != CIR_NODE(pattern));
	CHECK(copy->tree1_origin == CIR_NODE(pattern));

	// The copied child's placeholder datadef is substituted to the concrete int.
	node_t copied_leaf = c2mir_node_first_op(copy->as_node());
	REQUIRE(copied_leaf != nullptr);
	CHECK(CIR_NODE(copied_leaf)->datadef == &ddINT);

	// The ORIGINAL pattern is untouched (Tree-1 immutability): its child still
	// carries the placeholder.
	CHECK(CIR_NODE(leaf)->datadef == &T);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);
}

// Two-tree Phase 4 (widening): tsubst peels ptr/ref layers around a placeholder.
// A body node whose madc type is `T*` (resp. `T&`) must substitute to `int*`
// (resp. `int&`) from the same bare `T -> int` binding. Tree-1 stays untouched.
TEST_CASE("CIR: tsubst peels ptr/ref layers around a template parameter") {
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	auto prog = std::make_shared<Program>();
	CirBuilder builder(c2m);
	builder.translate_module(prog.get());	// primes m_prog (the type-builder owner)

	DataDefTemplateParam T("T", 0);
	DataDef *ptrT = prog->getPointerType(&T);
	DataDef *refT = prog->getReferenceType(&T);
	DataDef *ptrInt = prog->getPointerType(&ddINT);
	DataDef *refInt = prog->getReferenceType(&ddINT);

	node_t pleaf = builder.id("p");
	CIR_NODE(pleaf)->datadef = ptrT;
	node_t rleaf = builder.id("r");
	CIR_NODE(rleaf)->datadef = refT;
	node_t pattern = builder.node2(N_COMMA, pleaf, rleaf);

	std::map<DataDef *, DataDef *> subst;
	subst[&T] = &ddINT;

	cir_node *copy = builder.tsubst_cir(CIR_NODE(pattern), subst);
	REQUIRE(copy != nullptr);

	// T* -> int*, T& -> int& on the copy; the layers are rebuilt, not the bare T.
	node_t cp = c2mir_node_first_op(copy->as_node());
	REQUIRE(cp != nullptr);
	node_t cr = c2mir_node_next_op(cp);
	REQUIRE(cr != nullptr);
	CHECK(CIR_NODE(cp)->datadef == ptrInt);
	CHECK(CIR_NODE(cr)->datadef == refInt);

	// Tree-1 immutability: the originals still carry T* / T&.
	CHECK(CIR_NODE(pleaf)->datadef == ptrT);
	CHECK(CIR_NODE(rleaf)->datadef == refT);
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);
}

// Two-tree Phase 3 (widening): a DEFERRED TYPE-SPEC MARKER in a Tree-1 pattern —
// an N_IGNORE node carrying a template-parameter placeholder, left by
// append_type_specs in pattern mode where `T` is used as a TYPE (a `(T)x` cast,
// a `T tmp;` local) — is expanded by tsubst to the CONCRETE type's specs, using
// the same append_type_specs the re-parse path uses (so the result is identical).
TEST_CASE("CIR: tsubst expands a deferred type-spec marker to the concrete specs") {
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);
    {
	CirBuilder builder(c2m);

	DataDefTemplateParam T("T", 0);

	// A spec list holding a single deferred marker (the shape append_type_specs
	// emits for a placeholder in pattern mode).
	node_t specs = builder.list();
	node_t marker = builder.ignore();
	CIR_NODE(marker)->datadef = &T;
	builder.append(specs, marker);

	std::map<DataDef *, DataDef *> subst;
	subst[&T] = &ddINT;

	cir_node *copy = builder.tsubst_cir(CIR_NODE(specs), subst);
	REQUIRE(copy != nullptr);
	CHECK(cir_tree_has_error(copy->as_node()) == false);

	// The marker expanded to whatever append_type_specs(int) yields — compare
	// codes node-for-node so the test is robust to int's exact spec node code.
	node_t ref = builder.list();
	builder.append_type_specs(ref, &ddINT);

	node_t got = c2mir_node_first_op(copy->as_node());
	node_t want = c2mir_node_first_op(ref);
	REQUIRE(got != nullptr);
	REQUIRE(want != nullptr);
	CHECK(got->code == want->code);            // e.g. N_INT, not the N_IGNORE marker
	CHECK(got->code != N_IGNORE);
	// single-node scalar spec: no trailing extra nodes beyond the reference's.
	CHECK((c2mir_node_next_op(got) == nullptr) ==
	      (c2mir_node_next_op(want) == nullptr));
    }
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);
}
