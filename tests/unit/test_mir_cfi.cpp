// Unit tests for the DWARF call frame information MIR_gen describes for the
// code it generates (docs/plans/2026-10-19-table-driven-unwinding.md, step 2).
// The host's own unwinder is the oracle: a walk started in a host callback
// must pass through the MIR frames back to the C++ caller, and a C++
// exception thrown there must cross them and land in the caller's handler
// with the caller's registers intact.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <unwind.h>

extern "C" {
#include "mir.h"
#include "mir-gen.h"
}

// The targets whose generated frames are registered with the host unwinder
// (Windows describes frames with its own tables).
#if (defined(__x86_64__) || defined(__aarch64__) || defined(__PPC64__) || defined(__riscv) \
     || defined(__s390x__)) && !defined(_WIN32)
#define MIR_CFI_HOST 1
#endif

#ifdef MIR_CFI_HOST

// What the walk saw: each frame's IP, outermost last.
enum { MAX_FRAMES = 64 };
static uintptr_t walk_ips[MAX_FRAMES];
static int walk_len;
static bool throw_from_cb;

struct cfi_thrown {
	int64_t v;
};

static _Unwind_Reason_Code collect_ip(struct _Unwind_Context *uc, void *)
{
	if (walk_len == MAX_FRAMES)
		return _URC_END_OF_STACK;
	walk_ips[walk_len++] = (uintptr_t)_Unwind_GetIP(uc);
	return _URC_NO_REASON;
}

// Called from MIR code (h): walks the stack, then returns or throws.
extern "C" __attribute__((noinline)) int64_t cfi_cb(int64_t x)
{
	walk_len = 0;
	_Unwind_Backtrace(collect_ip, nullptr);
	if (throw_from_cb)
		throw cfi_thrown{x};
	return x * 2;
}

static void *cfi_import_resolver(const char *name)
{
	if (strcmp(name, "cfi_cb") == 0) return (void *)cfi_cb;
	return nullptr;
}

// f: a frame-pointer frame (it allocates) calling h. h: no ALLOCA -- an SP
// frame on x86-64 -- holding two values in callee-saved registers across its
// call of the host callback.
static const char *cfi_module_text =
	"m: module\n"
	"  import cfi_cb\n"
	"cb_p: proto i64, i64:x\n"
	"h_p: proto i64, i64:x\n"
	"h: func i64, i64:x\n"
	"  local i64:a, i64:b, i64:r\n"
	"  mul a, x, 3\n"
	"  add b, x, 5\n"
	"  call cb_p, cfi_cb, r, x\n"
	"  add r, r, a\n"
	"  add r, r, b\n"
	"  ret r\n"
	"  endfunc\n"
	"f: func i64, i64:x\n"
	"  local i64:base, i64:r\n"
	"  alloca base, 32\n"
	"  mov i64:8(base), x\n"
	"  call h_p, h, r, x\n"
	"  add r, r, i64:8(base)\n"
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

static bool in_code(uintptr_t ip, MIR_item_t func)
{
	uintptr_t start = (uintptr_t)func->u.func->machine_code;
	return start <= ip && ip < start + func->u.func->code_len;
}

typedef int64_t (*cfi_fn_t)(int64_t);

// The C++ caller of f: the walk must reach this frame.
static __attribute__((noinline)) int64_t call_f(cfi_fn_t fn, int64_t x)
{
	volatile int64_t r = fn(x);	// not a tail call: this frame stays

	return r;
}

// The C++ caller that catches what the callback throws through f and h. The
// values it holds across the call must survive the unwind.
static __attribute__((noinline)) int64_t catch_f(cfi_fn_t fn, int64_t x)
{
	volatile int64_t keep = x * 7;
	int64_t k1 = x + 11, k2 = x * 13;

	try {
		fn(x);
	} catch (const cfi_thrown &e) {
		return e.v + keep + k1 + k2;
	}
	return -1;
}

struct cfi_jit {
	MIR_context_t ctx;
	MIR_item_t f, h;
	cfi_fn_t fn;

	cfi_jit(int level)
	{
		ctx = MIR_init();
		MIR_gen_init(ctx);
		MIR_gen_set_optimize_level(ctx, level);
		MIR_set_inline_permission(ctx, 0);
		MIR_scan_string(ctx, cfi_module_text);
		MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
		f = find_item(ctx, mod, "f");
		h = find_item(ctx, mod, "h");
		MIR_load_module(ctx, mod);
		MIR_link(ctx, MIR_set_gen_interface, cfi_import_resolver);
		fn = (cfi_fn_t)MIR_gen(ctx, f);
	}
	~cfi_jit()
	{
		MIR_gen_finish(ctx);
		MIR_finish(ctx);
	}
};

// The walk from the callback passed its own frame, then h, f and call_f.
// Without a description of h's frame the host unwinder stops at h.
static bool walked_through(const cfi_jit &j)
{
	int i = 0;

	while (i < walk_len && !in_code(walk_ips[i], j.h))
		i++;
	if (i + 2 >= walk_len || !in_code(walk_ips[i + 1], j.f))
		return false;
	uintptr_t caller = walk_ips[i + 2], call_f_start = (uintptr_t)call_f;
	return call_f_start < caller && caller < call_f_start + 256;
}

TEST_SUITE("MIR CFI") {

TEST_CASE("the host unwinder walks through generated frames")
{
	for (int level = 0; level <= 2; level++) {
		CAPTURE(level);
		cfi_jit j(level);
		REQUIRE(j.f != nullptr);
		REQUIRE(j.h != nullptr);
		REQUIRE(j.fn != nullptr);
		throw_from_cb = false;
		// h: 2x + 3x + (x + 5); f adds x back
		CHECK(call_f(j.fn, 4) == 8 + 12 + 9 + 4);
		CHECK(walked_through(j));
	}
}

TEST_CASE("a C++ exception crosses generated frames")
{
	for (int level = 0; level <= 2; level++) {
		CAPTURE(level);
		cfi_jit j(level);
		REQUIRE(j.fn != nullptr);
		// the walk must pass first: an exception the unwinder cannot carry
		// through h terminates the process
		throw_from_cb = false;
		call_f(j.fn, 1);
		REQUIRE(walked_through(j));
		throw_from_cb = true;
		CHECK(catch_f(j.fn, 5) == 5 + 35 + 16 + 65);
		throw_from_cb = false;
	}
}

TEST_CASE("every generated function gets a frame description")
{
	cfi_jit j(1);
	MIR_cfi_cie_t cie;
	REQUIRE(MIR_gen_cfi_cie(&cie) == 0);
	CHECK(cie.code_align == 1);
	CHECK(cie.data_align == -8);
	CHECK(cie.insns_len > 0);
	REQUIRE(j.f->u.func->cfi != nullptr);
	REQUIRE(j.h->u.func->cfi != nullptr);
	CHECK(j.f->u.func->cfi_len > 0);
	CHECK(j.h->u.func->cfi_len > 0);
}

}

#else

TEST_CASE("MIR CFI: no host registration on this target")
{
	MIR_cfi_cie_t cie;
	(void)cie;
	CHECK(true);
}

#endif
