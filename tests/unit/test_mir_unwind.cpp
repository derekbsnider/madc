// Unit tests for MIR's unwind regions (docs/plans/2026-10-19-table-driven-
// unwinding.md): the per-call regions MIR_gen records and the frame base a
// stack walk computes from them (MIR_unwind_info_t). A test that runs the
// generated code checks the recorded numbers against the REAL frames: a host
// callback walks the stack with the host unwinder -- through the CFI MIR_gen
// registered -- matches each frame's return address against a function's
// recorded call ranges, and reads the region's variable at the frame's CFA
// (the calling context's _Unwind_GetCFA) + base_cfa_offset + the region's
// offset, the way the runtime's walk will.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <unwind.h>
#include <vector>

extern "C" {
#include "mir.h"
#include "mir-gen.h"
}

// The targets whose generated frames are registered with the host unwinder
// (Windows describes frames with its own tables).
#if (defined(__x86_64__) || defined(__aarch64__) || defined(__PPC64__) || defined(__riscv) \
     || defined(__s390x__)) && !defined(_WIN32)
#define MIR_UNWIND_HOST 1
#endif

#ifdef MIR_UNWIND_HOST

// The generated functions a walk looks for, and the frames it found in one of
// their regions: the region live at the call, the variable's address and what
// it held while the frame lived.
struct region_frame {
	MIR_item_t func;
	uint32_t region;
	char *var;
	int64_t val;
};
static std::vector<MIR_item_t> walk_funcs;
static std::vector<region_frame> found;

// The call of FUNC whose recorded range holds return address IP, or null.
static const MIR_unwind_call_t *call_at(MIR_item_t func, uintptr_t ip)
{
	const MIR_unwind_info_t &u = func->u.func->unwind;
	uintptr_t code = (uintptr_t)func->u.func->machine_code;
	for (size_t i = 0; i < u.ncalls; i++)
		if (ip > code + u.calls[i].start && ip <= code + u.calls[i].start + u.calls[i].len)
			return &u.calls[i];
	return nullptr;
}

static char *frame_base(const MIR_unwind_info_t &u, uintptr_t cfa)
{
	uintptr_t b = cfa + (intptr_t)u.base_cfa_offset;
	if (u.base_align > 1)
		b = (b + u.base_align - 1) & ~(uintptr_t)(u.base_align - 1);
	return (char *)b;
}

// The frame matched at the previous step. A context's _Unwind_GetCFA is the SP
// at its IP -- the CFA of the frame it CALLED (libgcc and LLVM libunwind
// alike) -- so a matched frame's own CFA is what the next, calling context
// reports.
static MIR_item_t pending_func;
static const MIR_unwind_call_t *pending_call;

static _Unwind_Reason_Code find_region_frame(struct _Unwind_Context *uc, void *)
{
	if (pending_func != nullptr) {
		const MIR_unwind_info_t &u = pending_func->u.func->unwind;
		MIR_unwind_region_t r = VARR_GET(MIR_unwind_region_t,
						 pending_func->u.func->unwind_regions,
						 pending_call->region - 1);
		char *var = frame_base(u, (uintptr_t)_Unwind_GetCFA(uc)) + r.offset;
		found.push_back({pending_func, pending_call->region, var, *(int64_t *)var});
		pending_func = nullptr;
	}
	uintptr_t ip = (uintptr_t)_Unwind_GetIP(uc);
	for (MIR_item_t func : walk_funcs) {
		const MIR_unwind_call_t *c = call_at(func, ip);
		if (c != nullptr && func->u.func->unwind.base_p) {
			pending_func = func;
			pending_call = c;
			break;
		}
	}
	return _URC_NO_REASON;
}

// Walk the stack from here while every frame lives.
static __attribute__((noinline)) void walk_regions(void)
{
	found.clear();
	pending_func = nullptr;
	_Unwind_Backtrace(find_region_frame, nullptr);
}

// What the callbacks saw of their MIR caller: the last call's return address
// and argument, and the argument of the last call that walked.
static void *seen_ra, *seen_arg, *walked_arg;

// Called from a MIR frame with a variable's address (or 0): walks the stack
// when it has one.
extern "C" __attribute__((noinline)) int64_t unwind_cb_fp(int64_t p)
{
	seen_ra = __builtin_return_address(0);
	seen_arg = (void *)p;
	if (p != 0) {
		walked_arg = seen_arg;
		walk_regions();
	}
	return 7;
}

// Called with eight integer arguments (two on the stack on SysV x86-64).
extern "C" __attribute__((noinline)) int64_t
unwind_cb_sp(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
	     int64_t a6, int64_t a7, int64_t a8)
{
	walk_regions();
	return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8;
}

extern "C" void unwind_cleanup(void *) {}

static void *unwind_import_resolver(const char *name)
{
	if (strcmp(name, "unwind_cb_fp") == 0) return (void *)unwind_cb_fp;
	if (strcmp(name, "unwind_cb_sp") == 0) return (void *)unwind_cb_sp;
	if (strcmp(name, "unwind_cleanup") == 0) return (void *)unwind_cleanup;
	return nullptr;
}

// f: its top ALLOCA is the frame base, and its first call is in a region.
// g: no ALLOCA, a call passing stack args. h: calls g in a region.
static const char *unwind_module_text =
	"m: module\n"
	"  import unwind_cb_fp, unwind_cb_sp, unwind_cleanup\n"
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

// Give FUNC a frame base (its `base` reg) and one region at OFFSET, live at
// its first call.
static void add_region(MIR_context_t ctx, MIR_module_t mod, MIR_item_t func,
		       int64_t offset)
{
	MIR_set_func_unwind_base(ctx, func, MIR_reg(ctx, "base", func->u.func), 0);
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
	// The walk needs no entry for a call without a region.
	REQUIRE(u.ncalls == 1);
	CHECK(u.calls[0].region == 1);
	CHECK(u.calls[0].len > 0);
	seen_arg = (void *)1;
	CHECK(fn(41) == 7);
	CHECK(seen_arg == nullptr);	// the second call (argument 0) ran last
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
}

TEST_CASE("the region's call: its range holds the return address; CFA + base + offset is the variable")
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
	REQUIRE(u.ncalls == 1);
	walk_funcs = {f};
	found.clear();
	walked_arg = nullptr;
	CHECK(fn(41) == 7);
	// The walk found f's frame at its region's call -- the variable f passed,
	// holding what f stored there.
	REQUIRE(found.size() == 1);
	CHECK(found[0].func == f);
	CHECK(found[0].region == 1);
	CHECK((void *)found[0].var == walked_arg);
	CHECK(found[0].val == 41);
	// seen_ra is from the second call (no region): outside the recorded range.
	CHECK(call_at(f, (uintptr_t)seen_ra) == nullptr);
	walk_funcs.clear();
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
}

TEST_CASE("a walk through a frame passing stack args reaches the region's frame")
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
	walk_funcs = {g, h};
	found.clear();
	hn(5);
	// Past g's frame (no region), h's frame base + the region's offset holds
	// what h stored there.
	REQUIRE(found.size() == 1);
	CHECK(found[0].func == h);
	CHECK(found[0].region == 1);
	CHECK(found[0].val == 5);
	walk_funcs.clear();
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
}

// Binary module I/O keeps the frame base, the regions and each insn's region.
static std::vector<uint8_t> io_bytes;
static size_t io_pos;
static int io_put(MIR_context_t, uint8_t b) { io_bytes.push_back(b); return 1; }
static int io_get(MIR_context_t)
{
	return io_pos < io_bytes.size() ? io_bytes[io_pos++] : EOF;
}

TEST_CASE("binary module I/O keeps unwind regions")
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
	CHECK(nth_call(f2, 0)->unwind_region == 1);
	CHECK(nth_call(f2, 1)->unwind_region == 0);
	MIR_finish(ctx2);
}

TEST_CASE("the inliner: a region's callee stays a call; an inlined body takes the call's region")
{
	static const char *text =
		"m2: module\n"
		"  import unwind_cb_fp, unwind_cleanup\n"
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

#else

TEST_CASE("MIR unwind regions: no host unwinder for generated frames")
{
	CHECK(true);
}

#endif
