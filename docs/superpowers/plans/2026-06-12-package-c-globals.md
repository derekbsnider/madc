# Package C increment 1: get/set_global on live MIR storage — Implementation Plan

> **STATUS: EXECUTED 2026-06-12** on `feature/eval-globals-claude` (4 commits).
> All three global tests unskipped (integer roundtrip, 64-bit preserve, STRING
> roundtrip — Task 4's reach check landed fully); neighbor canary added;
> test_libmadc_program 106/0/35. Beyond plan: dynamic global init restructured
> into the synthesized `__madc_global_init` module function (main calls it;
> call-only sessions run it at runtime init; static once-guard) — the c2mir
> declaration grammar is `N_SPEC_DECL(N_SHARE(specs), declarator, attrs, asm,
> init)` (c2mir.c:538), 5 children, miss it and codegen segfaults. Gates:
> fulltest 577/0/0/18 exit 0, both gates GREEN, zero warnings.

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (inline — no implementation subagents under Fable). Checkbox steps.

**Goal:** Unskip the get/set_global tests in `tests/unit/test_libmadc_program.cpp` (:1806 integer roundtrip, :1829 64-bit preserve; :1854 string global if the prior art reaches) by making the libmadc global accessors operate on the **JIT's live MIR data**, not the parser's `var->data` backing store.

**Root cause (grounded 2026-06-12):** `program::get_global`/`set_global` (`src/madc_program.cpp:4158/:4183`) are complete frameworks calling `value_from_variable`/`set_variable_from_value` (`:2179/:2206`), which dereference `var->data` — the parse-time calloc'd store. On CIR, compiled code reads/writes the MIR module's data items, so host writes are invisible to script calls (and vice versa). `CirJitSession` (`src/madc_cir.h:42`) has `function_code(emitted_name)` but no data-item lookup.

**Second latent bug, fix at the same layer:** `value_from_variable` reads EVERY integer global via `var->get<int64_t>()` and `set_variable_from_value` writes `ddINT` as int64 — 8-byte accesses on what is a 4-byte `int` in MIR data layout. Harmless on oversized parse-time storage; neighbor-clobbering on real layout. All accesses must honor `type->size`.

**Approach (deepest layer, no shims):**
1. `CirJitSession::data_address(const char *emitted_name)` — resolve a module data/bss item's runtime address by name (sibling of `function_code`).
2. The value↔storage helpers get a storage-pointer parameter (`value_from_storage(DataDef*, void*, value&)` form) so get/set_global can pass the **resolved MIR address** while every other caller keeps passing `var->data`. Do NOT rebind `var->data` itself — the parser owns that allocation and teardown would free MIR-owned memory.
3. Size-correct accesses by `type->size` in the storage helpers.

**Branch:** `feature/eval-globals-claude` off `develop`.

**Repo gotchas:** `git commit -F tmp/msg.txt`; capped runs `( ulimit -t 600; timeout 900 … )`; `make -C src test` is the ONLY target relinking `bin/test_*`; doctest output can be swallowed in `test_libmadc_program` (engine-owned IO) — on a silent CHECK failure build a host probe in `tmp/` linking `lib/libmadc.a` (`g++ -std=c++11 -I include tmp/probe.cpp lib/libmadc.a /workspace/mir/libmir.a -rdynamic -ldl -lz -lm -lpthread`) and fprintf(stderr).

---

### Task 0: Branch + emission recon

- [ ] **0.1** `git -C /workspace/madc checkout -b feature/eval-globals-claude`
- [ ] **0.2** Recon the MIR item shape for globals (name? data vs bss? mangled?). Write `tmp/probe_items.cpp`:

```cpp
// Probe: list MIR module items for a tiny program with globals.
#include "libmadc/engine.h"
#include "libmadc/program.h"
#include <cstdio>
int main()
{
	madc::program pgm;
	FILE *f = fopen("/workspace/madc/tmp/glob.mad", "w");
	fputs("int counter = 4;\nint zeroed;\nlong big = 7;\nint main() { return 0; }\n", f);
	fclose(f);
	if ( !pgm.compile_file("/workspace/madc/tmp/glob.mad") )
	{ fprintf(stderr, "compile failed\n"); return 1; }
	madc::value v;
	pgm.get_global("counter", &v);   // forces runtime init; ignore result
	return 0;
}
```

Temporarily add a DBG-style item walk OR (cheaper) grep how CirBuilder emits globals first: `grep -n "new_data\|new_bss\|MIR_new_data\|global" src/cir_builder.cpp | head -30` and inspect. Record findings (item kind + exact name) in this plan file before Task 1. If names are NOT the plain source identifier, Task 1 keys on whatever they are — `Variable` likely carries the emitted name (check `var_emit_name` / `emit_symbol` precedence via `CirBuilder::call_emit_symbol`'s data equivalent).

### Task 1: `CirJitSession::data_address()`

**Files:** `src/madc_cir.h` (class), `src/madc_cir.cpp` (impl), `tests/unit/test_cir.cpp` (doctest)

- [ ] **1.1 Failing test** — append to the existing suite in `tests/unit/test_cir.cpp` (it already builds sessions; follow its existing setup pattern for a Program — copy the nearest TEST_CASE's tokenize/parse boilerplate):

```cpp
TEST_CASE("CirJitSession resolves global data addresses by name") {
	// build a Program from: int counter = 4; int main(){return 0;}
	// (reuse the file-based setup pattern used by the neighboring cases)
	CirJitSession session;
	REQUIRE(session.build(prog, "tmp_glob.mad"));
	void *addr = session.data_address("counter");
	REQUIRE(addr != (void *)NULL);
	CHECK(*(int *)addr == 4);
	CHECK(session.data_address("no_such_global") == (void *)NULL);
}
```

- [ ] **1.2** Run capped `make -C src test` — expect compile failure (no member).
- [ ] **1.3 Implement** — `src/madc_cir.h` after `function_code`:

```cpp
    // The linked runtime address of a module DATA/BSS item by its emitted
    // name (globals emit under their source/emitted identifier). Valid
    // after build() (module loaded+linked). NULL when absent.
    void *data_address(const char *emitted_name);
```

`src/madc_cir.cpp` (next to `function_code`; iterate the module item DLIST the same way other walkers do — c2mir/MIR idiom):

```cpp
void *CirJitSession::data_address(const char *emitted_name)
{
	if ( !mod || !emitted_name )
		return NULL;
	for ( MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
	      item != NULL;
	      item = DLIST_NEXT(MIR_item_t, item) )
	{
		const char *name = NULL;
		if ( item->item_type == MIR_data_item && item->u.data->name )
			name = item->u.data->name;
		else if ( item->item_type == MIR_bss_item && item->u.bss->name )
			name = item->u.bss->name;
		else
			continue;
		if ( std::strcmp(name, emitted_name) == 0 )
			return item->addr;
	}
	return NULL;
}
```

(Verify field spellings against `/workspace/mir/mir.h` — `MIR_data_item`/`MIR_bss_item`, `u.data->name`, `item->addr` — adjust to the real API, e.g. `MIR_item_name` if provided. If `item->addr` is NULL until load: `build()` already loads+links; assert with the Task-0 probe.)

- [ ] **1.4** Run capped `make -C src test` — green. **1.5** Commit.

### Task 2: storage-pointer helpers + size-correct accesses

**Files:** `src/madc_program.cpp` (`value_from_variable` :2179, `set_variable_from_value` :2206 and their two other call sites :2317/:3212)

- [ ] **2.1 Refactor** the two helpers into storage-parameter forms, keeping the old signatures as thin wrappers (other call sites unchanged):

```cpp
bool value_from_storage(DataDef *type, void *data, size_t count, bool fixed_or_vla, value &out);
bool set_storage_from_value(DataDef *type, void *data, size_t count, bool fixed_or_vla, const value &in);
// value_from_variable(var, out) := value_from_storage(var->type, var->data, var->count, var->is_fixed_array()||var->is_vla(), out)
```

- [ ] **2.2 Size-correct integer reads** in `value_from_storage`: replace the blanket `var->get<int64_t>()` with a `type->size` switch (1/2/4/8 → int8/16/32/64, unsigned variants zero-extend via the matching uint read; pointers read as void*→int64). Mirror the existing write-side per-type dispatch, and fix the write side's `ddINT` case to write **4 bytes** (int32), not int64. `is_real`: float (4) vs double (8) by size.
- [ ] **2.3** Doctest the helpers' size discipline indirectly via Task 3's neighbor-canary test (no separate unit hook needed — the helpers are file-static).

### Task 3: get/set_global resolve MIR storage + unskip

**Files:** `src/madc_program.cpp` (:4158/:4183), `tests/unit/test_libmadc_program.cpp`

- [ ] **3.1** In both accessors, after the existing var checks, resolve the live address: `void *storage = jit ? jit->data_address(id.c_str()) : NULL; if ( !storage ) storage = var->data;` then call the storage-form helpers. (The `jit` member is the `std::unique_ptr<CirJitSession>` at :2485; confirm the emitted name == source identifier from Task 0 — if not, use the Variable's emitted-name accessor found in Task 0.)
- [ ] **3.2** Unskip :1806 and :1829 (remove `* doctest::skip()`).
- [ ] **3.3** Add a neighbor-canary TEST_CASE (the size-bug regression test): program `int a = 4; int b = 7; int read_b() { return b; }` — `set_global("a", 9)` then `call("read_b")` must still return 7, and `get_global("b")` must be 7.
- [ ] **3.4** Run capped `make -C src test` — green, no swallowed failures. **3.5** Commit.

### Task 4: string global (std::string) — prior-art reach check

- [ ] **4.1** Find how runtime-eval scope capture marshals **string locals** (landed 2026-06-11 eval track: "int/real/array/string locals" — grep `madc_program.cpp` for the capture path, e.g. `capture`, `scope_binding`, `value_text`, or the `marshals_value_text()` consumers). If it reads a live `std::string` object generically, reuse it in `value_from_storage` for class-typed globals whose DataDef `marshals_value_text()`; `set` side: construct/assign through the same mechanism the capture write-back uses (if any).
- [ ] **4.2** If reachable: implement + unskip :1854. If NOT cleanly reachable (e.g. capture only handles locals via call-site codegen, no host-side object access): leave :1854 skipped, record the precise blocker in this plan + the design doc, and do NOT shim (no hardcoded std::string layout reads — `scripts/check-no-std-hardcoding.sh` gates it anyway).
- [ ] **4.3** Commit whatever landed.

### Task 5: gates + handoff

- [ ] **5.1** Clean rebuild zero warnings; capped `make -C src test`.
- [ ] **5.2** `( ulimit -t 1200; timeout 1800 make -C src fulltest )` → **577/0/0/18 exit 0**, both gates GREEN (integration counts unchanged — this is unit-surface work; the unskipped tests raise `test_libmadc_program` passed-count, skip count 38→35 or 36).
- [ ] **5.3** Design-doc/plan status lines only (NO full mirror sync — user direction 2026-06-12, see memory `mirror-sync-cadence`). Hand off for user verification before merge.
