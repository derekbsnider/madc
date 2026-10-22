// Unit tests for MIR's unwind regions (docs/plans/2026-10-19-table-driven-
// unwinding.md): the per-call regions MIR_gen records, the region table a
// function's FDE hands its personality (MIR_UNWIND_TABLE_VERSION), the
// regions c2mir opens for cleanup variables, and the runtime's own personality
// (src/rt/rt_personality.c) and forced unwind (src/rt/rt_except.c). A test
// that runs the generated code raises an exception through the generated
// frames with the host unwinder (_Unwind_RaiseException), as the runtime's
// throw will: the unwinder
// calls the test's personality for each frame whose FDE names it, with that
// frame's region table; the personality finds the call site, the frame base
// (the table's register in that frame + its offset), and in the cleanup phase
// runs the region's cleanups through the table's pointers, then lands.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unwind.h>
#include <utility>
#include <vector>

extern "C" {
#include "mir.h"
#include "mir-gen.h"
#include "c2mir.h"
}
#include "mir-debug.h"
#include "../../src/rt/rt_except.h"

// The targets whose generated frames are registered with the host unwinder
// (Windows describes frames with its own tables).
#if (defined(__x86_64__) || defined(__aarch64__) || defined(__PPC64__) || defined(__riscv) \
     || defined(__s390x__)) && !defined(_WIN32)
#define MIR_UNWIND_HOST 1
#endif

#ifdef MIR_UNWIND_HOST

// A function's region table, as its personality reads it.
struct table_region {
	uint32_t parent;
	int64_t offset;
	uintptr_t cleanup;
};
struct region_table {
	uint32_t base_reg;
	int64_t base_offset;
	uint32_t base_align;
	std::vector<MIR_unwind_call_t> calls;
	std::vector<table_region> regions;
};

static uint64_t read_uleb(const uint8_t *&p)
{
	uint64_t v = 0;
	for (unsigned shift = 0;; shift += 7) {
		uint8_t byte = *p++;
		v |= (uint64_t)(byte & 0x7f) << shift;
		if ((byte & 0x80) == 0)
			return v;
	}
}

static int64_t read_sleb(const uint8_t *&p)
{
	int64_t v = 0;
	unsigned shift = 0;
	uint8_t byte;
	do {
		byte = *p++;
		v |= (int64_t)(byte & 0x7f) << shift;
		shift += 7;
	} while (byte & 0x80);
	if (shift < 64 && (byte & 0x40))
		v |= -((int64_t)1 << shift);
	return v;
}

// The table at P (the FDE's LSDA); false when it is not one MIR wrote with
// absolute pointers.
static bool read_table(const uint8_t *p, region_table &t)
{
	if (p == nullptr || *p++ != MIR_UNWIND_TABLE_VERSION || *p++ != 0x00 /* DW_EH_PE_absptr */)
		return false;
	t.base_reg = (uint32_t)read_uleb(p);
	t.base_offset = read_sleb(p);
	t.base_align = (uint32_t)read_uleb(p);
	t.calls.resize(read_uleb(p));
	for (MIR_unwind_call_t &c : t.calls) {
		c.start = (uint32_t)read_uleb(p);
		c.len = (uint32_t)read_uleb(p);
		c.region = (uint32_t)read_uleb(p);
	}
	t.regions.resize(read_uleb(p));
	for (table_region &r : t.regions) {
		r.parent = (uint32_t)read_uleb(p);
		r.offset = read_sleb(p);
		uint64_t cleanup;
		memcpy(&cleanup, p, sizeof(cleanup));
		p += sizeof(cleanup);
		r.cleanup = (uintptr_t)cleanup;
	}
	return true;
}

// What the personality saw: per search-phase frame, the function, the region
// live at its call, the region's variable and what it held.
struct region_frame {
	uintptr_t func;
	uint32_t region;
	char *var;
	int64_t val;
};
static std::vector<region_frame> found;
static int personality_calls;		// search-phase frames handed to the personality
static uintptr_t handler_func;		// the function whose frame lands the raise
static jmp_buf landing;
// What the cleanups were called with, in order: the address and what it held.
static std::vector<std::pair<void *, int64_t>> cleaned;

extern "C" _Unwind_Reason_Code
unwind_test_personality(int version, _Unwind_Action actions, _Unwind_Exception_Class,
			struct _Unwind_Exception *, struct _Unwind_Context *uc)
{
	region_table t;
	if (version != 1 || !read_table((const uint8_t *)_Unwind_GetLanguageSpecificData(uc), t))
		return _URC_FATAL_PHASE1_ERROR;
	uintptr_t func = _Unwind_GetRegionStart(uc), ra = _Unwind_GetIP(uc) - func;
	const MIR_unwind_call_t *call = nullptr;
	for (const MIR_unwind_call_t &c : t.calls)
		if (ra > c.start && ra <= c.start + c.len)
			call = &c;
	if (call == nullptr)
		return _URC_CONTINUE_UNWIND;
	uintptr_t base = (uintptr_t)_Unwind_GetGR(uc, (int)t.base_reg) + (intptr_t)t.base_offset;
	if (t.base_align > 1)
		base = (base + t.base_align - 1) & ~(uintptr_t)(t.base_align - 1);
	if (actions & _UA_SEARCH_PHASE) {
		personality_calls++;
		char *var = (char *)base + t.regions[call->region - 1].offset;
		found.push_back({func, call->region, var, *(int64_t *)var});
		return func == handler_func ? _URC_HANDLER_FOUND : _URC_CONTINUE_UNWIND;
	}
	for (uint32_t r = call->region; r != 0; r = t.regions[r - 1].parent)
		((void (*)(void *))t.regions[r - 1].cleanup)((char *)base + t.regions[r - 1].offset);
	if (actions & _UA_HANDLER_FRAME)
		longjmp(landing, 1);
	return _URC_CONTINUE_UNWIND;
}

static struct _Unwind_Exception test_exception;

// Raise through the generated frames; the personality lands in HANDLER's frame
// (its search phase must find it: a raise that returns is a failure).
static __attribute__((noinline)) void raise_through(void)
{
	memset(&test_exception, 0, sizeof(test_exception));
	test_exception.exception_class = 0x4d49522d54455354ULL;	// "MIR-TEST"
	_Unwind_RaiseException(&test_exception);
	FAIL("the raise returned: no frame was its handler");
}

// Run FN(ARG) with the raise landing in HANDLER's frame: true when it landed.
static bool run_landing(int64_t (*fn)(int64_t), int64_t arg, MIR_item_t handler)
{
	found.clear();
	cleaned.clear();
	personality_calls = 0;
	handler_func = (uintptr_t)handler->u.func->machine_code;
	if (setjmp(landing) == 0) {
		fn(arg);
		return false;
	}
	return true;
}

// What the callbacks saw of their MIR caller: the last call's return address
// and argument, and the argument of the last call that raised.
static void *seen_ra, *seen_arg, *raised_arg;
static bool raise_p;	// do the callbacks raise when they have an address

// Called from a MIR frame with a variable's address (or 0): raises when it has
// one and raise_p is set.
extern "C" __attribute__((noinline)) int64_t unwind_cb_fp(int64_t p)
{
	seen_ra = __builtin_return_address(0);
	seen_arg = (void *)p;
	if (p != 0 && raise_p) {
		raised_arg = seen_arg;
		raise_through();
	}
	return 7;
}

// Called with eight integer arguments (two on the stack on SysV x86-64).
extern "C" __attribute__((noinline)) int64_t
unwind_cb_sp(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
	     int64_t a6, int64_t a7, int64_t a8)
{
	if (raise_p)
		raise_through();
	return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8;
}

// A region's cleanup: records the variable it was given.
extern "C" void unwind_cleanup(void *p)
{
	cleaned.push_back({p, *(int64_t *)p});
}

static void *unwind_import_resolver(const char *name)
{
	if (strcmp(name, "unwind_cb_fp") == 0) return (void *)unwind_cb_fp;
	if (strcmp(name, "unwind_cb_sp") == 0) return (void *)unwind_cb_sp;
	if (strcmp(name, "unwind_cleanup") == 0) return (void *)unwind_cleanup;
	if (strcmp(name, "unwind_record") == 0) return (void *)unwind_cleanup;
	if (strcmp(name, "unwind_test_personality") == 0) return (void *)unwind_test_personality;
	return nullptr;
}

// f: its top ALLOCA is the frame base, and its first call is in a region.
// g: no ALLOCA, a call passing stack args. h: calls g in a region.
static const char *unwind_module_text =
	"m: module\n"
	"  import unwind_cb_fp, unwind_cb_sp, unwind_cleanup, unwind_test_personality\n"
	"cb_p: proto i64, i64:p\n"
	"cb8_p: proto i64, i64:a1, i64:a2, i64:a3, i64:a4, i64:a5, i64:a6, i64:a7, i64:a8\n"
	"g_p: proto i64, i64:x\n"
	"f: func i64, i64:a\n"
	"  local i64:base, i64:r, i64:q\n"
	"  alloca base, 40\n"
	"  mov i64:24(base), a\n"
	"  add q, base, 24\n"
	"  call cb_p, unwind_cb_fp, r, q\n"
	"  call cb_p, unwind_cb_fp, r, 0\n"
	"  ret r\n"
	"  endfunc\n"
	"g: func i64, i64:x\n"
	"  local i64:r\n"
	"  call cb8_p, unwind_cb_sp, r, x, 2, 3, 4, 5, 6, 7, 8\n"
	"  ret r\n"
	"  endfunc\n"
	"h: func i64, i64:a\n"
	"  local i64:base, i64:r, i64:q\n"
	"  alloca base, 32\n"
	"  mov i64:8(base), a\n"
	"  add q, base, 8\n"
	"  call g_p, g, r, q\n"
	"  ret r\n"
	"  endfunc\n"
	"  endmodule\n";

static MIR_item_t find_item(MIR_context_t ctx, MIR_module_t mod, const char *name)
{
	for (MIR_item_t it = DLIST_HEAD(MIR_item_t, mod->items); it != nullptr;
	     it = DLIST_NEXT(MIR_item_t, it))
		if (MIR_item_name(ctx, it) != nullptr
		    && strcmp(MIR_item_name(ctx, it), name) == 0)
			return it;
	return nullptr;
}

// Does FUNC contain an ALLOCA (after linking: an inlined body brings its own)?
static bool has_alloca(MIR_item_t func)
{
	for (MIR_insn_t in = DLIST_HEAD(MIR_insn_t, func->u.func->insns);
	     in != nullptr; in = DLIST_NEXT(MIR_insn_t, in))
		if (in->code == MIR_ALLOCA)
			return true;
	return false;
}

// The Nth call insn of a function (0-based), before linking.
static MIR_insn_t nth_call(MIR_item_t func, int n)
{
	for (MIR_insn_t in = DLIST_HEAD(MIR_insn_t, func->u.func->insns);
	     in != nullptr; in = DLIST_NEXT(MIR_insn_t, in))
		if (MIR_call_code_p(in->code) && n-- == 0)
			return in;
	return nullptr;
}

// Give FUNC a frame base (its `base` reg), the test personality (unless
// PERSONALITY is false), and one region at OFFSET, live at its first call.
static void add_region(MIR_context_t ctx, MIR_module_t mod, MIR_item_t func,
		       int64_t offset, bool personality = true)
{
	MIR_set_func_unwind_base(ctx, func, MIR_reg(ctx, "base", func->u.func), 0);
	if (personality)
		MIR_set_func_unwind_personality(ctx, func,
						find_item(ctx, mod, "unwind_test_personality"));
	uint32_t r = MIR_new_unwind_region(ctx, func, 0, offset,
					   find_item(ctx, mod, "unwind_cleanup"));
	CHECK(r == 1);
	nth_call(func, 0)->unwind_region = r;
}

static MIR_module_t scan_module(MIR_context_t ctx)
{
	MIR_scan_string(ctx, unwind_module_text);
	return DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
}

static MIR_context_t gen_context(void)
{
	MIR_context_t ctx = MIR_init();
	MIR_gen_init(ctx);
	MIR_gen_set_optimize_level(ctx, 1);
	MIR_set_inline_permission(ctx, 0);
	return ctx;
}

TEST_SUITE("MIR unwind regions") {

TEST_CASE("a function records only the calls in a region")
{
	MIR_context_t ctx = gen_context();
	MIR_module_t mod = scan_module(ctx);
	MIR_item_t f = find_item(ctx, mod, "f");
	REQUIRE(f != nullptr);
	add_region(ctx, mod, f, 24);
	MIR_load_module(ctx, mod);
	MIR_link(ctx, MIR_set_gen_interface, unwind_import_resolver);
	int64_t (*fn)(int64_t) = (int64_t (*)(int64_t))MIR_gen(ctx, f);
	REQUIRE(fn != nullptr);
	const MIR_unwind_info_t &u = f->u.func->unwind;
	CHECK(u.valid_p);
	CHECK(u.base_p);
	// The table needs no entry for a call without a region.
	REQUIRE(u.ncalls == 1);
	CHECK(u.calls[0].region == 1);
	CHECK(u.calls[0].len > 0);
	raise_p = false;
	seen_arg = (void *)1;
	CHECK(fn(41) == 7);
	CHECK(seen_arg == nullptr);	// the second call (argument 0) ran last
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
}

TEST_CASE("a raise through the region's call: the personality gets the table, the variable and runs its cleanup")
{
	MIR_context_t ctx = gen_context();
	MIR_module_t mod = scan_module(ctx);
	MIR_item_t f = find_item(ctx, mod, "f");
	REQUIRE(f != nullptr);
	add_region(ctx, mod, f, 24);
	MIR_load_module(ctx, mod);
	MIR_link(ctx, MIR_set_gen_interface, unwind_import_resolver);
	int64_t (*fn)(int64_t) = (int64_t (*)(int64_t))MIR_gen(ctx, f);
	REQUIRE(fn != nullptr);
	raise_p = true;
	raised_arg = nullptr;
	REQUIRE(run_landing(fn, 41, f));
	raise_p = false;
	// The search phase found f's frame at its region's call -- the variable f
	// passed, holding what f stored there -- and f's frame was its handler.
	REQUIRE(found.size() == 1);
	CHECK(found[0].func == (uintptr_t)f->u.func->machine_code);
	CHECK(found[0].region == 1);
	CHECK((void *)found[0].var == raised_arg);
	CHECK(found[0].val == 41);
	// The cleanup phase called the region's cleanup with that variable.
	REQUIRE(cleaned.size() == 1);
	CHECK(cleaned[0].first == raised_arg);
	CHECK(cleaned[0].second == 41);
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
}

TEST_CASE("regions without a personality: no table, the unwinder passes the frame by")
{
	MIR_context_t ctx = gen_context();
	MIR_module_t mod = scan_module(ctx);
	MIR_item_t f = find_item(ctx, mod, "f");
	REQUIRE(f != nullptr);
	add_region(ctx, mod, f, 24, false);
	MIR_load_module(ctx, mod);
	MIR_link(ctx, MIR_set_gen_interface, unwind_import_resolver);
	int64_t (*fn)(int64_t) = (int64_t (*)(int64_t))MIR_gen(ctx, f);
	REQUIRE(fn != nullptr);
	REQUIRE(f->u.func->unwind.ncalls == 1);	// the regions are recorded all the same
	found.clear();
	cleaned.clear();
	personality_calls = 0;
	handler_func = (uintptr_t)f->u.func->machine_code;
	raise_p = true;
	bool caught = false;
	try {
		fn(41);
	} catch (...) {	// a foreign exception: only catch (...) takes it
		caught = true;
	}
	raise_p = false;
	CHECK(caught);
	CHECK(personality_calls == 0);
	CHECK(cleaned.empty());
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
}

TEST_CASE("a raise through a frame passing stack args: only the frame with regions names the personality")
{
	MIR_context_t ctx = gen_context();
	MIR_module_t mod = scan_module(ctx);
	MIR_item_t g = find_item(ctx, mod, "g"), h = find_item(ctx, mod, "h");
	REQUIRE(g != nullptr);
	REQUIRE(h != nullptr);
	add_region(ctx, mod, h, 8);
	MIR_load_module(ctx, mod);
	MIR_link(ctx, MIR_set_gen_interface, unwind_import_resolver);
	REQUIRE(MIR_gen(ctx, g) != nullptr);
	int64_t (*hn)(int64_t) = (int64_t (*)(int64_t))MIR_gen(ctx, h);
	REQUIRE(hn != nullptr);
	CHECK(g->u.func->unwind.valid_p);
	CHECK(g->u.func->unwind.ncalls == 0);
	REQUIRE(h->u.func->unwind.base_p);
	raise_p = true;
	REQUIRE(run_landing(hn, 5, h));
	raise_p = false;
	// g's frame (no region, no personality) was passed by; h's frame base +
	// the region's offset holds what h stored there.
	CHECK(personality_calls == 1);
	REQUIRE(found.size() == 1);
	CHECK(found[0].func == (uintptr_t)h->u.func->machine_code);
	CHECK(found[0].region == 1);
	CHECK(found[0].val == 5);
	REQUIRE(cleaned.size() == 1);
	CHECK(cleaned[0].first == (void *)found[0].var);
	CHECK(cleaned[0].second == 5);
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
}

// Binary module I/O keeps the frame base, the regions, the personality and
// each insn's region.
static std::vector<uint8_t> io_bytes;
static size_t io_pos;
static int io_put(MIR_context_t, uint8_t b) { io_bytes.push_back(b); return 1; }
static int io_get(MIR_context_t)
{
	return io_pos < io_bytes.size() ? io_bytes[io_pos++] : EOF;
}

TEST_CASE("binary module I/O keeps unwind regions and the personality")
{
	MIR_context_t ctx = MIR_init();
	MIR_module_t mod = scan_module(ctx);
	MIR_item_t f = find_item(ctx, mod, "f");
	add_region(ctx, mod, f, 24);
	io_bytes.clear();
	MIR_write_module_with_func(ctx, io_put, mod);
	MIR_finish(ctx);

	MIR_context_t ctx2 = MIR_init();
	io_pos = 0;
	MIR_read_with_func(ctx2, io_get);
	MIR_module_t mod2 = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx2));
	MIR_item_t f2 = find_item(ctx2, mod2, "f");
	REQUIRE(f2 != nullptr);
	MIR_func_t fn2 = f2->u.func;
	CHECK(fn2->unwind_base_reg == MIR_reg(ctx2, "base", fn2));
	REQUIRE(fn2->unwind_regions != nullptr);
	REQUIRE(VARR_LENGTH(MIR_unwind_region_t, fn2->unwind_regions) == 1);
	MIR_unwind_region_t r = VARR_GET(MIR_unwind_region_t, fn2->unwind_regions, 0);
	CHECK(r.parent == 0);
	CHECK(r.offset == 24);
	CHECK(strcmp(MIR_item_name(ctx2, r.cleanup), "unwind_cleanup") == 0);
	REQUIRE(fn2->unwind_personality != nullptr);
	CHECK(strcmp(MIR_item_name(ctx2, fn2->unwind_personality), "unwind_test_personality") == 0);
	CHECK(find_item(ctx2, mod2, "g")->u.func->unwind_personality == nullptr);
	CHECK(nth_call(f2, 0)->unwind_region == 1);
	CHECK(nth_call(f2, 1)->unwind_region == 0);
	MIR_finish(ctx2);
}

// A label belongs to the insn after it: the stream's region change comes
// ahead of the labels, so a region that starts or ends at a labelled insn
// reads back (the reader refused a region change between a label and its
// insn: "uregion should have no labels").
static jmp_buf io_error_jmp;
static void MIR_NO_RETURN io_error(MIR_error_type_t, const char *, ...)
{
	longjmp(io_error_jmp, 1);
}

TEST_CASE("binary module I/O: a region changing at a labelled insn")
{
	MIR_context_t ctx = MIR_init();
	MIR_module_t mod = scan_module(ctx);
	MIR_item_t f = find_item(ctx, mod, "f");
	add_region(ctx, mod, f, 24);
	MIR_insert_insn_before(ctx, f, nth_call(f, 0), MIR_new_label(ctx));
	MIR_insert_insn_before(ctx, f, nth_call(f, 1), MIR_new_label(ctx));
	MIR_insert_insn_before(ctx, f, nth_call(f, 1), MIR_new_label(ctx));
	io_bytes.clear();
	MIR_write_module_with_func(ctx, io_put, mod);
	MIR_finish(ctx);

	MIR_context_t ctx2 = MIR_init();
	MIR_set_error_func(ctx2, io_error);
	io_pos = 0;
	bool read_ok = false;
	if (setjmp(io_error_jmp) == 0) {
		MIR_read_with_func(ctx2, io_get);
		read_ok = true;
	}
	REQUIRE(read_ok);
	MIR_module_t mod2 = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx2));
	MIR_item_t f2 = find_item(ctx2, mod2, "f");
	REQUIRE(f2 != nullptr);
	CHECK(nth_call(f2, 0)->unwind_region == 1);
	CHECK(nth_call(f2, 1)->unwind_region == 0);
	MIR_insn_t before = DLIST_PREV(MIR_insn_t, nth_call(f2, 1));
	REQUIRE(before != nullptr);
	CHECK(before->code == MIR_LABEL);
	MIR_finish(ctx2);
}

// A read that fails part way leaves the module it began unfinished —
// MIR_new_module and MIR_finish both refuse that state. MIR_abandon_module
// discards it: the context builds its next module (the same names again) and
// finishes. The failure part way: the compressed stream decodes a full 256 KiB
// buffer before its end's hash is checked, so a module larger than one buffer
// whose stream lost that hash is begun, then refused.
static std::string many_functions_module(int n)
{
	std::string text = "big: module\n";
	for (int i = 0; i < n; i++)
		text += "f" + std::to_string(i) + ": func i64, i64:a\n  local i64:t\n"
			"  mul t, a, " + std::to_string(i) + "\n  add t, t, 1\n  ret t\n  endfunc\n";
	return text + "endmodule\n";
}

TEST_CASE("binary module I/O: a failed read's module is abandoned, the context goes on")
{
	MIR_context_t ctx = MIR_init();
	MIR_scan_string(ctx, many_functions_module(20000).c_str());
	MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
	io_bytes.clear();
	MIR_write_module_with_func(ctx, io_put, mod);
	MIR_finish(ctx);
	REQUIRE(io_bytes.size() > 9u);
	io_bytes.resize(io_bytes.size() - 9);	// the end: its zero tag and 8-byte hash

	MIR_context_t ctx2 = MIR_init();
	MIR_set_error_func(ctx2, io_error);
	io_pos = 0;
	volatile bool read_ok = false;
	if (setjmp(io_error_jmp) == 0) {
		MIR_read_with_func(ctx2, io_get);
		read_ok = true;
	}
	REQUIRE_FALSE(read_ok);
	// The read had begun its module.
	REQUIRE(DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx2)) != nullptr);
	MIR_abandon_module(ctx2);
	CHECK(DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx2)) == nullptr);
	MIR_abandon_module(ctx2);	// nothing unfinished: no effect

	bool built = false;
	MIR_module_t next = nullptr;
	if (setjmp(io_error_jmp) == 0) {
		next = scan_module(ctx2);
		built = true;
	}
	REQUIRE(built);
	REQUIRE(next != nullptr);
	CHECK(find_item(ctx2, next, "f") != nullptr);
	bool finished = false;
	if (setjmp(io_error_jmp) == 0) {
		MIR_finish(ctx2);
		finished = true;
	}
	CHECK(finished);
}

TEST_CASE("the inliner: a region's callee stays a call; an inlined body takes the call's region")
{
	static const char *text =
		"m2: module\n"
		"  import unwind_cb_fp, unwind_cleanup, unwind_test_personality\n"
		"cb_p: proto i64, i64:p\n"
		"k_p: proto i64, i64:x\n"
		"k: func i64, i64:x\n"
		"  local i64:r\n"
		"  call cb_p, unwind_cb_fp, r, x\n"
		"  ret r\n"
		"  endfunc\n"
		"kk: func i64, i64:a\n"
		"  local i64:base, i64:r, i64:q\n"
		"  alloca base, 16\n"
		"  mov i64:0(base), a\n"
		"  add q, base, 0\n"
		"  inline k_p, k, r, q\n"
		"  ret r\n"
		"  endfunc\n"
		"rr: func i64, i64:a\n"
		"  local i64:base, i64:r, i64:q\n"
		"  alloca base, 16\n"
		"  mov i64:0(base), a\n"
		"  add q, base, 0\n"
		"  call cb_p, unwind_cb_fp, r, q\n"
		"  ret r\n"
		"  endfunc\n"
		"cc: func i64, i64:a\n"
		"  local i64:r\n"
		"  inline k_p, rr, r, a\n"
		"  ret r\n"
		"  endfunc\n"
		"  endmodule\n";
	MIR_context_t ctx = MIR_init();
	MIR_gen_init(ctx);
	MIR_scan_string(ctx, text);
	MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
	MIR_item_t kk = find_item(ctx, mod, "kk"), rr = find_item(ctx, mod, "rr"),
		   cc = find_item(ctx, mod, "cc");
	add_region(ctx, mod, kk, 0);	// the inline call to k
	add_region(ctx, mod, rr, 0);	// rr declares a region: never inlined
	MIR_load_module(ctx, mod);
	MIR_link(ctx, MIR_set_gen_interface, unwind_import_resolver);
	// kk: k's body was inlined, and its call to the callback is in kk's region.
	MIR_insn_t c = nth_call(kk, 0);
	REQUIRE(c != nullptr);
	CHECK(c->unwind_region == 1);
	CHECK(nth_call(kk, 1) == nullptr);
	CHECK(has_alloca(kk));
	// cc: the call to rr stays a call -- an inlined rr would have brought
	// its ALLOCA along (k has none, so kk's is its own).
	CHECK(nth_call(cc, 0) != nullptr);
	CHECK(!has_alloca(cc));
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
}

}

// c2mir: with c2mir_options.unwind_regions_p, an automatic
// __attribute__((cleanup)) variable is an unwind region from the end of its
// declaration (or its __builtin_cleanup_arm) to the end of its scope, and its
// own cleanup call runs in the region it nests in; a function with a region
// names c2mir_options.unwind_personality.

struct C2mirSource {
	const char *text;
	size_t pos;
};

static int c2mir_source_getc(void *data)
{
	C2mirSource *s = (C2mirSource *)data;
	return s->text[s->pos] == '\0' ? EOF : (unsigned char)s->text[s->pos++];
}

// Compile C text with or without unwind regions; the module it made.
static MIR_module_t compile_c(MIR_context_t ctx, const char *text, bool regions)
{
	struct c2mir_options opts;
	memset(&opts, 0, sizeof(opts));
	opts.message_file = stderr;
	opts.unwind_regions_p = regions;
	opts.unwind_personality = regions ? "unwind_test_personality" : nullptr;
	C2mirSource src = {text, 0};
	if (!c2mir_compile(ctx, &opts, c2mir_source_getc, &src, "unwind.c", nullptr))
		return nullptr;
	return DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
}

// The region of FUNC's Nth call (0-based) to CALLEE, or -1 when there is none.
static int64_t call_region(MIR_context_t ctx, MIR_item_t func, const char *callee, int n)
{
	for (MIR_insn_t in = DLIST_HEAD(MIR_insn_t, func->u.func->insns);
	     in != nullptr; in = DLIST_NEXT(MIR_insn_t, in)) {
		if (!MIR_call_code_p(in->code) || in->ops[1].mode != MIR_OP_REF)
			continue;
		const char *name = MIR_item_name(ctx, in->ops[1].u.ref);
		if (name != nullptr && strcmp(name, callee) == 0 && n-- == 0)
			return in->unwind_region;
	}
	return -1;
}

static MIR_unwind_region_t region_of(MIR_item_t func, uint32_t region)
{
	return VARR_GET(MIR_unwind_region_t, func->u.func->unwind_regions, region - 1);
}

static const char *c2mir_unwind_text =
	"struct S { long v; };\n"
	"long unwind_cb_fp (long p);\n"
	"void unwind_record (long *p);\n"
	"static void s_done (struct S *p) { unwind_record (&p->v); p->v = 0; }\n"
	"static void s_make (struct S *p, long v) { p->v = v; }\n"
	"static void x_done (long *p) { unwind_record (p); *p = 0; }\n"
	"long nested (long a) {\n"
	"  long r = unwind_cb_fp (0);\n"
	"  {\n"
	"    struct S outer __attribute__ ((cleanup (s_done))) = {a};\n"
	"    {\n"
	"      struct S inner __attribute__ ((cleanup (s_done))) = {a + 1};\n"
	"      r += unwind_cb_fp ((long) &inner.v);\n"
	"    }\n"
	"    r += unwind_cb_fp (0);\n"
	"  }\n"
	"  return r + unwind_cb_fp (0);\n"
	"}\n"
	"long armed (long a) {\n"
	"  struct S s __attribute__ ((cleanup (s_done)));\n"
	"  s_make (&s, a);\n"
	"  __builtin_cleanup_arm (&s);\n"
	"  return unwind_cb_fp ((long) &s.v);\n"
	"}\n"
	"long scalar (long a) {\n"
	"  long x __attribute__ ((cleanup (x_done))) = a;\n"
	"  return unwind_cb_fp ((long) &x);\n"
	"}\n";

TEST_SUITE("c2mir unwind regions") {

TEST_CASE("nested cleanup variables: regions, their parents, the calls in each, and a raise")
{
	MIR_context_t ctx = gen_context();
	c2mir_init(ctx);
	MIR_module_t mod = compile_c(ctx, c2mir_unwind_text, true);
	REQUIRE(mod != nullptr);
	MIR_item_t f = find_item(ctx, mod, "nested");
	REQUIRE(f != nullptr);
	REQUIRE(f->u.func->unwind_regions != nullptr);
	REQUIRE(VARR_LENGTH(MIR_unwind_region_t, f->u.func->unwind_regions) == 2);
	MIR_unwind_region_t outer = region_of(f, 1), inner = region_of(f, 2);
	CHECK(outer.parent == 0);
	CHECK(inner.parent == 1);
	CHECK(strcmp(MIR_item_name(ctx, outer.cleanup), "s_done") == 0);
	CHECK(outer.offset != inner.offset);
	// a function with a region names the personality; one without does not
	REQUIRE(f->u.func->unwind_personality != nullptr);
	CHECK(strcmp(MIR_item_name(ctx, f->u.func->unwind_personality), "unwind_test_personality") == 0);
	CHECK(find_item(ctx, mod, "s_make")->u.func->unwind_personality == nullptr);
	// before the block, inside the inner block, after it, after the outer
	CHECK(call_region(ctx, f, "unwind_cb_fp", 0) == 0);
	CHECK(call_region(ctx, f, "unwind_cb_fp", 1) == 2);
	CHECK(call_region(ctx, f, "unwind_cb_fp", 2) == 1);
	CHECK(call_region(ctx, f, "unwind_cb_fp", 3) == 0);
	// each variable's own cleanup call runs in the region it nests in
	CHECK(call_region(ctx, f, "s_done", 0) == 1);	// inner's
	CHECK(call_region(ctx, f, "s_done", 1) == 0);	// outer's
	MIR_load_module(ctx, mod);
	MIR_link(ctx, MIR_set_gen_interface, unwind_import_resolver);
	int64_t (*fn)(int64_t) = (int64_t (*)(int64_t))MIR_gen(ctx, f);
	REQUIRE(fn != nullptr);
	REQUIRE(f->u.func->unwind.base_p);
	// the personality's view: inner's region at the call, and the frame base
	// + inner's offset is the variable the call passed, holding its
	// initializer; the cleanup phase runs inner's cleanup, then outer's
	raise_p = true;
	raised_arg = nullptr;
	REQUIRE(run_landing(fn, 41, f));
	raise_p = false;
	REQUIRE(found.size() == 1);
	CHECK(found[0].region == 2);
	CHECK((void *)found[0].var == raised_arg);
	CHECK(found[0].val == 42);
	REQUIRE(cleaned.size() == 2);
	CHECK(cleaned[0].first == raised_arg);
	CHECK(cleaned[0].second == 42);
	CHECK(cleaned[1].first != raised_arg);
	CHECK(cleaned[1].second == 41);
	c2mir_finish(ctx);
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
}

TEST_CASE("__builtin_cleanup_arm: the region starts after the construction")
{
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	MIR_module_t mod = compile_c(ctx, c2mir_unwind_text, true);
	REQUIRE(mod != nullptr);
	MIR_item_t f = find_item(ctx, mod, "armed");
	REQUIRE(f != nullptr);
	REQUIRE(f->u.func->unwind_regions != nullptr);
	REQUIRE(VARR_LENGTH(MIR_unwind_region_t, f->u.func->unwind_regions) == 1);
	CHECK(call_region(ctx, f, "s_make", 0) == 0);	// not constructed yet
	CHECK(call_region(ctx, f, "unwind_cb_fp", 0) == 1);
	CHECK(call_region(ctx, f, "s_done", 0) == 0);	// the return's cleanup
	c2mir_finish(ctx);
	MIR_finish(ctx);
}

TEST_CASE("a scalar cleanup variable lives in the frame, at its region's offset")
{
	MIR_context_t ctx = gen_context();
	c2mir_init(ctx);
	MIR_module_t mod = compile_c(ctx, c2mir_unwind_text, true);
	REQUIRE(mod != nullptr);
	MIR_item_t f = find_item(ctx, mod, "scalar");
	REQUIRE(f != nullptr);
	REQUIRE(f->u.func->unwind_regions != nullptr);
	CHECK(call_region(ctx, f, "unwind_cb_fp", 0) == 1);
	MIR_load_module(ctx, mod);
	MIR_link(ctx, MIR_set_gen_interface, unwind_import_resolver);
	int64_t (*fn)(int64_t) = (int64_t (*)(int64_t))MIR_gen(ctx, f);
	REQUIRE(fn != nullptr);
	REQUIRE(f->u.func->unwind.base_p);
	raise_p = true;
	raised_arg = nullptr;
	REQUIRE(run_landing(fn, 41, f));
	raise_p = false;
	REQUIRE(found.size() == 1);
	CHECK(found[0].region == 1);
	CHECK((void *)found[0].var == raised_arg);
	CHECK(found[0].val == 41);
	REQUIRE(cleaned.size() == 1);
	CHECK(cleaned[0].first == raised_arg);
	CHECK(cleaned[0].second == 41);
	c2mir_finish(ctx);
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
}

TEST_CASE("without the option: no regions, no personality, the arm is a no-op")
{
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	MIR_module_t mod = compile_c(ctx, c2mir_unwind_text, false);
	REQUIRE(mod != nullptr);
	for (const char *name : {"nested", "armed", "scalar"}) {
		MIR_item_t f = find_item(ctx, mod, name);
		REQUIRE(f != nullptr);
		CHECK(f->u.func->unwind_regions == nullptr);
		CHECK(f->u.func->unwind_base_reg == 0);
		CHECK(f->u.func->unwind_personality == nullptr);
	}
	CHECK(call_region(ctx, find_item(ctx, mod, "armed"), "unwind_cb_fp", 0) == 0);
	c2mir_finish(ctx);
	MIR_finish(ctx);
}

TEST_CASE("__builtin_cleanup_arm of a variable without a cleanup does nothing; of a non-variable, an error")
{
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	MIR_module_t mod = compile_c(ctx,
		"long unwind_cb_fp (long p);\n"
		"long plain (long a) {\n"
		"  long x = a;\n"
		"  static long s;\n"
		"  __builtin_cleanup_arm (&x);\n"
		"  __builtin_cleanup_arm (&s);\n"
		"  return unwind_cb_fp ((long) &x) + s;\n"
		"}\n", true);
	REQUIRE(mod != nullptr);
	MIR_item_t f = find_item(ctx, mod, "plain");
	REQUIRE(f != nullptr);
	CHECK(f->u.func->unwind_regions == nullptr);
	CHECK(f->u.func->unwind_personality == nullptr);
	CHECK(compile_c(ctx, "void bad (void) { __builtin_cleanup_arm (0); }\n", true) == nullptr);
	c2mir_finish(ctx);
	MIR_finish(ctx);
}

}

// The runtime's personality (src/rt/rt_personality.c): madc's own try shape -- the
// try context, its setjmp, the guard whose cleanup is __madc_try_leave --
// compiled by c2mir with regions naming __madc_personality, and a throw
// through __madc_throw_int, which force-unwinds to the try. Only the runtime
// functions' addresses are taken here: the personality and the guard's leave
// are not on rt_except.h's host-callable surface.
extern "C" int __madc_personality(int, int, uint64_t, void *, void *);
extern "C" void __madc_try_leave(void *);
extern "C" int64_t __madc_exception_int(void);

static std::vector<int64_t> recorded;	// what each cleanup saw, in order

extern "C" void rt_record(int64_t *p)
{
	recorded.push_back(*p);
}

// A g++ frame with a live destructor between a throw and its try: g++ runs it
// from a landing pad, which resets SP to this frame and resumes the unwind with
// _Unwind_Resume (its own contexts on the stack below) -- so nothing the unwind
// needs may live on the stack below this frame.
struct RtHostObject {
	int64_t v;
	~RtHostObject() { recorded.push_back(v); }
};

extern "C" __attribute__((noinline)) void rt_host_layer(int64_t v)
{
	RtHostObject o{v + 400};
	__madc_throw_int(v);
}

static void *rt_import_resolver(const char *name)
{
	if (strcmp(name, "rt_record") == 0) return (void *)rt_record;
	if (strcmp(name, "rt_host_layer") == 0) return (void *)rt_host_layer;
	if (strcmp(name, "_setjmp") == 0) return (void *)_setjmp;
	if (strcmp(name, "__madc_try_push") == 0) return (void *)__madc_try_push;
	if (strcmp(name, "__madc_try_leave") == 0) return (void *)__madc_try_leave;
	if (strcmp(name, "__madc_throw_int") == 0) return (void *)__madc_throw_int;
	if (strcmp(name, "__madc_exception_int") == 0) return (void *)__madc_exception_int;
	if (strcmp(name, "__madc_exception_clear") == 0) return (void *)__madc_exception_clear;
	if (strcmp(name, "__madc_personality") == 0) return (void *)__madc_personality;
	return nullptr;
}

static const char *rt_try_text =
	"struct S { long v; };\n"
	"void rt_record (long *p);\n"
	"void *__madc_try_push (void *ctx);\n"
	"void __madc_try_leave (void *guard);\n"
	"void __madc_throw_int (long v);\n"
	"long __madc_exception_int (void);\n"
	"void __madc_exception_clear (void);\n"
	"int _setjmp (void *env);\n"
	"static void s_done (struct S *p) { rt_record (&p->v); }\n"
	"static void thrower (long v) {\n"
	"  struct S t __attribute__ ((cleanup (s_done))) = {v + 100};\n"
	"  __madc_throw_int (v);\n"
	"}\n"
	"void rt_host_layer (long v);\n"
	"static void through_host (long v) {\n"
	"  struct S t __attribute__ ((cleanup (s_done))) = {v + 200};\n"
	"  rt_host_layer (v);\n"
	"}\n"
	"long caught (long a) {\n"
	"  struct S before __attribute__ ((cleanup (s_done))) = {a};\n"
	"  long ctx[128];\n"
	"  if (_setjmp (__madc_try_push (ctx)) == 0) {\n"
	"    void *guard __attribute__ ((cleanup (__madc_try_leave))) = ctx;\n"
	"    struct S inside __attribute__ ((cleanup (s_done))) = {a + 1};\n"
	"    thrower (a + 2);\n"
	"    return -1;\n"
	"  }\n"
	"  long v = __madc_exception_int ();\n"
	"  __madc_exception_clear ();\n"
	"  return v;\n"
	"}\n"
	"long caught_host (long a) {\n"
	"  long ctx[128];\n"
	"  if (_setjmp (__madc_try_push (ctx)) == 0) {\n"
	"    void *guard __attribute__ ((cleanup (__madc_try_leave))) = ctx;\n"
	"    struct S inside __attribute__ ((cleanup (s_done))) = {a + 1};\n"
	"    through_host (a + 2);\n"
	"    return -1;\n"
	"  }\n"
	"  long v = __madc_exception_int ();\n"
	"  __madc_exception_clear ();\n"
	"  return v;\n"
	"}\n";

// Compile rt_try_text (with or without regions), run FUNC(A): its result.
static int64_t run_caught(bool regions, int64_t a, const char *func = "caught")
{
	MIR_context_t ctx = gen_context();
	c2mir_init(ctx);
	struct c2mir_options opts;
	memset(&opts, 0, sizeof(opts));
	opts.message_file = stderr;
	opts.unwind_regions_p = regions;
	opts.unwind_personality = regions ? "__madc_personality" : nullptr;
	C2mirSource src = {rt_try_text, 0};
	REQUIRE(c2mir_compile(ctx, &opts, c2mir_source_getc, &src, "rt_try.c", nullptr));
	MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
	MIR_item_t f = find_item(ctx, mod, func);
	REQUIRE(f != nullptr);
	MIR_load_module(ctx, mod);
	MIR_link(ctx, MIR_set_gen_interface, rt_import_resolver);
	int64_t (*fn)(int64_t) = (int64_t (*)(int64_t))MIR_gen(ctx, f);
	REQUIRE(fn != nullptr);
	recorded.clear();
	int64_t r = fn(a);
	c2mir_finish(ctx);
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
	return r;
}

TEST_SUITE("madc runtime personality") {

TEST_CASE("a throw force-unwinds to its try: the frames' cleanups run, the try body's stop at its guard")
{
	CHECK(run_caught(true, 10) == 12);
	// thrower's object, then the try body's -- both during the unwind -- then
	// `before`, declared ahead of the try, at the function's own exit
	REQUIRE(recorded.size() == 3);
	CHECK(recorded[0] == 112);
	CHECK(recorded[1] == 11);
	CHECK(recorded[2] == 10);
}

TEST_CASE("without regions no frame names the personality: the same throw lands with no frame cleanups")
{
	CHECK(run_caught(false, 10) == 12);
	REQUIRE(recorded.size() == 1);
	CHECK(recorded[0] == 10);
}

TEST_CASE("a g++ frame between the throw and the try runs its destructor and resumes the unwind")
{
	// the host frame's object (its landing pad and _Unwind_Resume), then the
	// generated caller's, then the try body's
	CHECK(run_caught(true, 10, "caught_host") == 12);
	REQUIRE(recorded.size() == 3);
	CHECK(recorded[0] == 412);
	CHECK(recorded[1] == 212);
	CHECK(recorded[2] == 11);
	// without regions the host frame's destructor still runs: the forced
	// unwind reaches every g++ frame
	CHECK(run_caught(false, 10, "caught_host") == 12);
	REQUIRE(recorded.size() == 1);
	CHECK(recorded[0] == 412);
}

}

// The AOT images: the same frames reach the platform unwinder through the
// image's .eh_frame -- registered by the in-process loader (ELF), found through
// PT_GNU_EH_FRAME in an ELF executable and through __unwind_info in a Mach-O
// one -- each FDE pointing at its region table in .gcc_except_table, whose
// cleanup pointers (like the CIE's personality, gcc's DW.ref word) go through
// .mir.addrpool slots: encoding 0x9b.
#if (defined(__linux__) || defined(__APPLE__)) && (defined(__x86_64__) || defined(__aarch64__))

struct AotUnit {
	const char *text;
	const char *personality;	// the regions name it; nullptr = no regions
};

// Object mode leaves every import an undefined symbol of the capture (madc's
// cir_object_import_resolver).
static void *object_import_sentinel(const char *)
{
	static char sentinel;
	return &sentinel;
}

// UNITS, each a c2mir translation unit, linked in object mode: every
// function is captured. The caller emits, then calls aot_finish.
static MIR_context_t aot_context(std::initializer_list<AotUnit> units)
{
	MIR_context_t ctx = gen_context();
	MIR_gen_set_object_mode(ctx, 1);
	c2mir_init(ctx);
	int n = 0;
	for (const AotUnit &u : units) {
		struct c2mir_options opts;
		memset(&opts, 0, sizeof(opts));
		opts.message_file = stderr;
		opts.unwind_regions_p = u.personality != nullptr;
		opts.unwind_personality = u.personality;
		C2mirSource src = {u.text, 0};
		char name[32];
		snprintf(name, sizeof(name), "aot%d.c", n++);
		REQUIRE(c2mir_compile(ctx, &opts, c2mir_source_getc, &src, name, nullptr));
	}
	for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); m != nullptr;
	     m = DLIST_NEXT(MIR_module_t, m))
		MIR_load_module(ctx, m);
	MIR_link(ctx, MIR_set_gen_interface, object_import_sentinel);
	return ctx;
}

static void aot_finish(MIR_context_t ctx)
{
	c2mir_finish(ctx);
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
}

static std::vector<uint8_t> emit_object(std::initializer_list<AotUnit> units)
{
	MIR_context_t ctx = aot_context(units);
	void *buf = nullptr;
	size_t size = 0;
	REQUIRE(MIR_gen_object_emit(ctx, &buf, &size) == 0);
	std::vector<uint8_t> bytes((uint8_t *)buf, (uint8_t *)buf + size);
	free(buf);
	aot_finish(ctx);
	return bytes;
}

#if defined(__linux__)
// A second translation unit for the merge: its own frames with regions.
static const char *rt_other_text =
	"struct O { long v; };\n"
	"void rt_record (long *p);\n"
	"static void o_done (struct O *p) { rt_record (&p->v); }\n"
	"long other (long a) {\n"
	"  struct O o __attribute__ ((cleanup (o_done))) = {a};\n"
	"  rt_record (&o.v);\n"
	"  return a;\n"
	"}\n";

static void *rt_object_resolver(const char *name, void *)
{
	return rt_import_resolver(name);
}

// rt_try_text (with or without regions) as an ELF relocatable -- with MERGE,
// read into a fresh builder after another object, so every section and
// relocation of it rebases -- loaded in-process; FUNC(A): its result.
static int64_t run_caught_aot(bool regions, int64_t a, const char *func, bool merge)
{
	const char *personality = regions ? "__madc_personality" : nullptr;
	std::vector<uint8_t> bytes = emit_object({{rt_try_text, personality}});
	char err[256] = "";
	if (merge) {
		std::vector<uint8_t> first = emit_object({{rt_other_text, personality}});
		MIR_object_t obj = MIR_object_create();
		REQUIRE(obj != nullptr);
		INFO("MIR_object_read: " << err);
		REQUIRE(MIR_object_read(obj, first.data(), first.size(), err, sizeof(err)) == 0);
		REQUIRE(MIR_object_read(obj, bytes.data(), bytes.size(), err, sizeof(err)) == 0);
		void *buf = nullptr;
		size_t size = 0;
		REQUIRE(MIR_object_emit(obj, &buf, &size) == 0);
		bytes.assign((uint8_t *)buf, (uint8_t *)buf + size);
		free(buf);
		MIR_object_destroy(obj);
	}
	MIR_object_loaded_t lo = MIR_object_load(bytes.data(), bytes.size(), rt_object_resolver,
						 nullptr, err, sizeof(err));
	INFO("MIR_object_load: " << err);
	REQUIRE(lo != nullptr);
	int64_t (*fn)(int64_t) = (int64_t (*)(int64_t))MIR_object_loaded_sym(lo, func);
	REQUIRE(fn != nullptr);
	recorded.clear();
	int64_t r = fn(a);
	MIR_object_loaded_unload(lo);
	return r;
}
#endif

// An executable's own personality and stop function (libc + libgcc_s only):
// the personality counts the frames that name it and the ones whose table it
// reads with the image encoding; the forced unwind's end prints both.
static const char *exe_personality_text =
	"int printf (const char *, ...);\n"
	"void exit (int);\n"
	"void *_Unwind_GetLanguageSpecificData (void *);\n"
	"static int calls, tables;\n"
	"int aot_personality (int version, int actions, unsigned long cls, void *ex, void *uc) {\n"
	"  const unsigned char *t = _Unwind_GetLanguageSpecificData (uc);\n"
	"  calls++;\n"
	"  if (t != 0 && t[0] == 1 && t[1] == 0x9b) tables++;\n"
	"  return 8; /* _URC_CONTINUE_UNWIND */\n"
	"}\n"
	"int aot_stop (int version, int actions, unsigned long cls, void *ex, void *uc, void *arg) {\n"
	"  if (actions & 16) { /* _UA_END_OF_STACK */\n"
	"    printf (\"%d %d\\n\", calls, tables);\n"
	"    exit (0);\n"
	"  }\n"
	"  return 0; /* _URC_NO_REASON */\n"
	"}\n";

static const char *exe_program_text =
	"int _Unwind_ForcedUnwind (void *, void *, void *);\n"
	"int aot_stop (int, int, unsigned long, void *, void *, void *);\n"
	"static void done (long *p) { (void) p; }\n"
	"static _Alignas (16) long ex[4];\n"
	"static void thrower (long v) {\n"
	"  long t __attribute__ ((cleanup (done))) = v;\n"
	"  _Unwind_ForcedUnwind (ex, aot_stop, 0);\n"
	"}\n"
	"static void mid (long v) {\n"
	"  long t __attribute__ ((cleanup (done))) = v;\n"
	"  thrower (v + 1);\n"
	"}\n"
	"int main (void) { mid (1); return 1; }\n";

// The executable's libraries: libc and libgcc_s on Linux; a Mach-O image
// always links libSystem, which carries the unwinder.
static MIR_object_exec_params exec_params()
{
	MIR_object_exec_params params;
	memset(&params, 0, sizeof(params));
#if defined(__linux__)
	static const char *const needed[] = {"libgcc_s.so.1", "libc.so.6"};
	params.needed = needed;
	params.n_needed = 2;
#endif
	return params;
}

// The executable image BUF (SIZE bytes, freed here), run: its output.
static std::string run_image(void *buf, size_t size)
{
	char path[] = "/tmp/madc_unwind_exe_XXXXXX";
	int fd = mkstemp(path);
	REQUIRE(fd >= 0);
	bool written = write(fd, buf, size) == (ssize_t)size;
	close(fd);
	free(buf);
	std::string out;
	if (written && chmod(path, 0755) == 0) {
		FILE *p = popen(path, "r");
		char line[256];
		while (p != nullptr && fgets(line, sizeof(line), p) != nullptr) out += line;
		if (p != nullptr) CHECK(pclose(p) == 0);
	}
	unlink(path);
	CHECK(written);
	return out;
}

// UNITS as an executable, run: its output.
static std::string run_executable(std::initializer_list<AotUnit> units)
{
	MIR_context_t ctx = aot_context(units);
	MIR_object_exec_params params = exec_params();
	void *buf = nullptr;
	size_t size = 0;
	REQUIRE(MIR_gen_object_emit_executable(ctx, &params, &buf, &size) == 0);
	aot_finish(ctx);
	return run_image(buf, size);
}

// Each of UNITS as its own object, the objects read into one builder, which
// emits the executable: every unwind record and field went through the
// container's .o and its reader. Run: its output.
static std::string run_merged_executable(std::initializer_list<AotUnit> units)
{
	std::vector<std::vector<uint8_t>> objects;
	for (const AotUnit &u : units) objects.push_back(emit_object({u}));
	MIR_object_t obj = MIR_object_create();
	REQUIRE(obj != nullptr);
	char err[256] = "";
	for (const std::vector<uint8_t> &bytes : objects) {
		INFO("MIR_object_read: " << err);
		REQUIRE(MIR_object_read(obj, bytes.data(), bytes.size(), err, sizeof(err)) == 0);
	}
	MIR_object_exec_params params = exec_params();
	void *buf = nullptr;
	size_t size = 0;
	REQUIRE(MIR_object_emit_executable(obj, &params, &buf, &size) == 0);
	MIR_object_destroy(obj);
	return run_image(buf, size);
}

TEST_SUITE("AOT unwind tables") {

#if defined(__linux__)
TEST_CASE("an object loaded in-process: its frames reach the unwinder, the personality runs their regions")
{
	CHECK(run_caught_aot(true, 10, "caught", false) == 12);
	REQUIRE(recorded.size() == 3);
	CHECK(recorded[0] == 112);
	CHECK(recorded[1] == 11);
	CHECK(recorded[2] == 10);
	// a g++ frame between the throw and the try
	CHECK(run_caught_aot(true, 10, "caught_host", false) == 12);
	REQUIRE(recorded.size() == 3);
	CHECK(recorded[0] == 412);
	CHECK(recorded[1] == 212);
	CHECK(recorded[2] == 11);
	// without regions every frame has its FDE and none names the personality
	CHECK(run_caught_aot(false, 10, "caught", false) == 12);
	REQUIRE(recorded.size() == 1);
	CHECK(recorded[0] == 10);
}

TEST_CASE("merged objects: the concatenated .eh_frame and region tables rebase")
{
	CHECK(run_caught_aot(true, 10, "caught", true) == 12);
	REQUIRE(recorded.size() == 3);
	CHECK(recorded[0] == 112);
	CHECK(recorded[1] == 11);
	CHECK(recorded[2] == 10);
}

#endif

// ELF: PT_GNU_EH_FRAME; Mach-O: __unwind_info
TEST_CASE("an executable: the unwinder finds its frames through the image's unwind index")
{
	// thrower and mid name the personality, and their tables are read with
	// the image encoding; main has no regions
	CHECK(run_executable({{exe_personality_text, nullptr},
			      {exe_program_text, "aot_personality"}})
	      == "2 2\n");
}

TEST_CASE("an executable from merged objects: the records survive the .o and its reader")
{
	CHECK(run_merged_executable({{exe_personality_text, nullptr},
				     {exe_program_text, "aot_personality"}})
	      == "2 2\n");
}

}

#endif

#else

TEST_CASE("MIR unwind regions: no host unwinder for generated frames")
{
	CHECK(true);
}

#endif
