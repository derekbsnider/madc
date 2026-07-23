// Unit tests for c2mir + MIR integration.
//
// Verifies that we can compile C text through c2mir, link it with
// MIR, generate machine code, and execute it. This is Phase 0
// validation — proving the c2mir → MIR → execute pipeline works
// before building the C emitter.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <string>
#include <cstring>

extern "C" {
#include "mir.h"
#include "mir-gen.h"
#include "c2mir.h"
}

// Feed a C string to c2mir via getc callback
struct StringReader {
    const char *data;
    size_t pos;
    size_t len;
};

static int string_getc(void *data) {
    StringReader *r = (StringReader *)data;
    if (r->pos >= r->len) return EOF;
    return (unsigned char)r->data[r->pos++];
}

// Import resolver for external functions
static void *test_import_resolver(const char *name) {
    if (strcmp(name, "printf") == 0) return (void *)(uintptr_t)printf;
    return nullptr;
}

// Compile C text, generate, and return function pointer
static void *compile_c_func(MIR_context_t ctx, const char *c_source,
			     const char *func_name) {
    struct c2mir_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.message_file = stderr;

    StringReader reader = {c_source, 0, strlen(c_source)};
    int ok = c2mir_compile(ctx, &opts, string_getc, &reader,
			    "test.c", nullptr);
    if (!ok) return nullptr;

    // Find and load the module
    MIR_module_t mod = nullptr;
    for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx));
	 m != nullptr;
	 m = DLIST_NEXT(MIR_module_t, m)) {
	mod = m;  // take the last module
    }
    if (!mod) return nullptr;

    MIR_load_module(ctx, mod);
    MIR_link(ctx, MIR_set_gen_interface, test_import_resolver);

    // Find the function
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
	 item != nullptr;
	 item = DLIST_NEXT(MIR_item_t, item)) {
	if (item->item_type == MIR_func_item &&
	    strcmp(item->u.func->name, func_name) == 0) {
	    return MIR_gen(ctx, item);
	}
    }
    return nullptr;
}

TEST_SUITE("c2mir → MIR pipeline") {

    TEST_CASE("Compile and run: return constant") {
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	MIR_gen_init(ctx);

	const char *src = "int answer(void) { return 42; }\n";
	typedef int (*fn_t)(void);
	fn_t fn = (fn_t)compile_c_func(ctx, src, "answer");
	REQUIRE(fn != nullptr);
	CHECK(fn() == 42);

	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
    }

    TEST_CASE("Compile and run: arithmetic") {
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	MIR_gen_init(ctx);

	const char *src =
	    "int add(int a, int b) { return a + b; }\n";
	typedef int (*fn_t)(int, int);
	fn_t fn = (fn_t)compile_c_func(ctx, src, "add");
	REQUIRE(fn != nullptr);
	CHECK(fn(3, 4) == 7);
	CHECK(fn(-10, 10) == 0);
	CHECK(fn(100, 200) == 300);

	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
    }

    TEST_CASE("Compile and run: loop (sieve-like)") {
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	MIR_gen_init(ctx);

	const char *src =
	    "int sum_to(int n) {\n"
	    "    int s = 0;\n"
	    "    for (int i = 1; i <= n; i++) s += i;\n"
	    "    return s;\n"
	    "}\n";
	typedef int (*fn_t)(int);
	fn_t fn = (fn_t)compile_c_func(ctx, src, "sum_to");
	REQUIRE(fn != nullptr);
	CHECK(fn(10) == 55);
	CHECK(fn(100) == 5050);
	CHECK(fn(0) == 0);

	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
    }

    TEST_CASE("Compile and run: double arithmetic") {
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	MIR_gen_init(ctx);

	const char *src =
	    "double avg(double a, double b) { return (a + b) / 2.0; }\n";
	typedef double (*fn_t)(double, double);
	fn_t fn = (fn_t)compile_c_func(ctx, src, "avg");
	REQUIRE(fn != nullptr);
	CHECK(fn(3.0, 5.0) == doctest::Approx(4.0));
	CHECK(fn(0.0, 10.0) == doctest::Approx(5.0));

	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
    }

    TEST_CASE("Compile and run: struct access") {
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	MIR_gen_init(ctx);

	const char *src =
	    "struct Point { int x; int y; };\n"
	    "int sum_point(struct Point *p) { return p->x + p->y; }\n";
	struct Point { int x; int y; };
	typedef int (*fn_t)(Point *);
	fn_t fn = (fn_t)compile_c_func(ctx, src, "sum_point");
	REQUIRE(fn != nullptr);
	Point p = {10, 20};
	CHECK(fn(&p) == 30);

	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
    }

    TEST_CASE("Compile and run: function calling extern") {
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	MIR_gen_init(ctx);

	// Call an external function (strlen via import resolver)
	const char *src =
	    "extern long strlen(const char *);\n"
	    "int slen(const char *s) { return (int)strlen(s); }\n";

	// Add strlen to imports
	typedef int (*fn_t)(const char *);
	// We need strlen in the resolver
	struct c2mir_options opts;
	memset(&opts, 0, sizeof(opts));
	opts.message_file = stderr;
	StringReader reader = {src, 0, strlen(src)};
	c2mir_compile(ctx, &opts, string_getc, &reader, "test.c", nullptr);

	MIR_module_t mod = nullptr;
	for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx));
	     m != nullptr; m = DLIST_NEXT(MIR_module_t, m))
	    mod = m;

	MIR_load_module(ctx, mod);
	// Use dlsym-based resolver for strlen
	MIR_link(ctx, MIR_set_gen_interface,
	    [](const char *name) -> void * {
		if (strcmp(name, "strlen") == 0) return (void *)(uintptr_t)strlen;
		return nullptr;
	    });

	fn_t fn = nullptr;
	for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
	     item != nullptr; item = DLIST_NEXT(MIR_item_t, item)) {
	    if (item->item_type == MIR_func_item &&
		strcmp(item->u.func->name, "slen") == 0) {
		fn = (fn_t)MIR_gen(ctx, item);
		break;
	    }
	}
	REQUIRE(fn != nullptr);
	CHECK(fn("hello") == 5);
	CHECK(fn("") == 0);
	CHECK(fn("madc transpiler") == 15);

	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
    }

    TEST_CASE("Compile and run: conditional") {
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	MIR_gen_init(ctx);

	const char *src =
	    "int max(int a, int b) { return a > b ? a : b; }\n";
	typedef int (*fn_t)(int, int);
	fn_t fn = (fn_t)compile_c_func(ctx, src, "max");
	REQUIRE(fn != nullptr);
	CHECK(fn(3, 5) == 5);
	CHECK(fn(10, 2) == 10);
	CHECK(fn(7, 7) == 7);

	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
    }
}
