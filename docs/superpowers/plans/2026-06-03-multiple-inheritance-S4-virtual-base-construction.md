# Multiple/Virtual Inheritance — S4: Virtual-Base Construction & Destruction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Construct and destroy a shared virtual base **exactly once, from the most-derived object, in the correct order** — fixing the diamond bug where madc constructs/destroys the vbase once per inheritance path.

**Architecture:** The Itanium "complete-object vs base-object" distinction. **Construction (site-based):** ordinary ctors construct only their *non-virtual* direct bases; the complete-object construction site (stack var-decl / `new`) constructs the transitive, deduped *virtual* bases first (base-most order), then runs the ctor. **Destruction (complete-dtor split):** the existing `Cls___dtor` becomes the *base-object* dtor (members + non-virtual bases, skipping vbases); a new `Cls___dtor_complete` (emitted only for classes with vbases) calls the base dtor then destroys the transitive deduped vbases (reverse order). The cleanup attribute on a complete object targets `_dtor_complete` (falling back to `_dtor` when there are no vbases); derived dtors call their bases' base-object `_dtor`.

**Tech Stack:** C++11; `src/cir_builder.cpp` (ctor prologue, `class_ctor_call`, the `new` path, `synth_dtor_def`, the cleanup-attribute dtor-symbol selection); `DataDefCLASS` (`collect_vbases`, `vbase_offset`, `base_offset_of` from S1–S3); doctest + integration `.mad` compared to g++.

**Spec:** `docs/superpowers/specs/2026-06-03-multiple-inheritance-design.md` Part 2 §6 (VTT/construction vtables) + §8 (ctor/dtor ordering). **Scope (explicit):** S4 here = vbase **construct/destruct exactly once + correct order** (the demonstrable diamond bug). **Deferred** (narrow, no current test/SMAUG/campaign consumer; their own follow-on): (i) **construction vtables** — a base ctor that makes a *virtual call during construction* seeing the under-construction vtable; (ii) **runtime `vbase_offset`** — dispatching a *virtual method declared in a virtual base* through a base pointer whose most-derived type is unknown. Both are logged as S4-follow-ons. Single inheritance and non-virtual MI stay byte-identical.

**Current bug (verified vs g++):**
- Construct: madc `T+L+T+R+D+`  →  g++ `T+L+R+D+` (Top twice, wrong order).
- Destroy:   madc `D-R-T-L-T-`  →  g++ `D-R-L-T-` (Top twice).
- Cause: S3 ctor/dtor chaining iterates **all** `bases` including virtual ones, so each intermediate (L, R) constructs/destroys the shared vbase.

**Live anchors (verified):**
- Ctor prologue base loop (S3 task 1): `src/cir_builder.cpp` ~7104-7112 (`if (is_ctor) for (... ocls->bases ...) base_call_at(...)`).
- Dtor epilogue base loop (S3 task 1): ~7126-7134 (reverse).
- `class_ctor_call`: `src/cir_builder.cpp:3579` (now also emits vptr-init for ctorless polymorphic classes — S3 crash fix).
- `synth_dtor_def`: ~3419-3461 (reverse `bases` loop with offset-adjusted `this`).
- The `new` no-user-ctor vptr-init: ~4573-4596; `new` ctor call path nearby.
- Cleanup-attribute dtor symbol: `class_dtor_symbol(cdd)` — search `class_dtor_symbol` + `cleanup(`.
- `collect_vbases(out, seen)` (parser.cpp), `vbase_offset` map, `base_offset_of` — all from S1–S3.

---

## File Structure
- **`src/cir_builder.cpp`**:
  - New helper `void vbase_ctor_stmts(node_t obj_addr, DataDefCLASS *cdd, std::vector<node_t> &out, TokenBase *o)` — append a ctor call (offset-adjusted to `vbase_offset[vb]`) for each transitive deduped vbase, base-most first. Used by `class_ctor_call` (stack) and the `new` path.
  - New helper `void vbase_dtor_stmts(node_t obj_addr, DataDefCLASS *cdd, std::vector<node_t> &out, TokenBase *o)` — reverse-order vbase dtor calls. Used by `Cls___dtor_complete`.
  - Ctor prologue + `class_ctor_call` + `synth_dtor_def`: **skip virtual bases** (`if (b.is_virtual) continue;`) — vbases handled at the complete level.
  - `class_ctor_call` / `new`: prepend `vbase_ctor_stmts` before the ctor body when `cdd` has vbases.
  - New `synth_complete_dtor_def(cdd)` emitting `Cls___dtor_complete`; emitted in Pass 1.6 alongside `synth_dtor_def` for classes with vbases.
  - `class_complete_dtor_symbol(cdd)` returning `_dtor_complete` if `cdd` has vbases else `class_dtor_symbol(cdd)`; the cleanup attribute uses it.
- **`tests/test_mi_vbase_ctor.mad`** (create) — the diamond ctor/dtor order, `.expect` = g++.

---

## Task 1: Construct virtual bases once, at the complete-object site

**Files:** Modify `src/cir_builder.cpp` (ctor prologue ~7104; `class_ctor_call` 3579; `new` path ~4573). Test: `tests/test_mi_vbase_ctor.mad`.

- [ ] **Step 1: Write the failing test**

`tests/test_mi_vbase_ctor.mad`:
```cpp
#!/../bin/madc
// Shared virtual base Top must be constructed ONCE (most-derived, first) and
// destroyed ONCE (last) — not once per diamond path.
class Top { public: Top(){printf("T+");} ~Top(){printf("T-");} };
class L : virtual public Top { public: L(){printf("L+");} ~L(){printf("L-");} };
class R : virtual public Top { public: R(){printf("R+");} ~R(){printf("R-");} };
class Diamond : public L, public R { public: Diamond(){printf("D+");} ~Diamond(){printf("D-");} };
int main() { { Diamond d; } printf("\n"); return 0; }
```
`tests/test_mi_vbase_ctor.expect`:
```
T+L+R+D+D-R-L-T-
```

- [ ] **Step 2: Run to verify it fails**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_vbase_ctor.mad )`
Expected: `T+L+T+R+D+D-R-T-L-T-` (Top constructed/destroyed twice).

- [ ] **Step 3: Add `vbase_ctor_stmts`; skip vbases in ctor chaining; construct vbases at the site**

Add the helper near `class_ctor_call` in `src/cir_builder.cpp` (declare in `cir_builder.h`):
```cpp
// Append a ctor call for each transitive, deduped virtual base of `cdd`, base-most
// first, with `this` adjusted to vbase_offset[vb]. `obj_addr` is a node yielding the
// most-derived object's address (e.g. &v or the new'd pointer).
void CirBuilder::vbase_ctor_stmts(node_t obj_addr, DataDefCLASS *cdd,
				  std::vector<node_t> &out, TokenBase *o)
{
	std::vector<DataDefCLASS *> vbs;
	std::set<DataDefCLASS *> seen;
	cdd->collect_vbases(vbs, seen);
	for (DataDefCLASS *vb : vbs) {
		if (!vb->has_user_ctor) continue;            // trivial vbase: nothing to run
		size_t off = cdd->vbase_offset.count(vb) ? cdd->vbase_offset[vb] : 0;
		node_t self = obj_addr;
		node_t adj = self;
		if (off != 0) {
			node_t charp = node2(N_CAST,
				node2(N_TYPE, node1(N_LIST, simple(N_CHAR)),
				      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
				self, o);
			adj = node2(N_ADD, charp, integer((long)off, o), o);
		}
		node_t vt = node2(N_TYPE,
			node1(N_LIST, node2(N_STRUCT, id(vb->name.c_str()), ignore())),
			node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		std::string sym = vb->name + "__" + vb->name;
		referenced_funcs.insert(sym);
		node_t a = list();
		append(a, node2(N_CAST, vt, adj, o));
		out.push_back(node2(N_EXPR, list(), node2(N_CALL, id(sym.c_str(), o), a, o), o));
	}
}
```
In the **ctor prologue** base loop (~7104-7112), skip virtual bases:
```cpp
		if (is_ctor)
			for (size_t bi = 0; bi < ocls->bases.size(); bi++) {
				if (ocls->bases[bi].is_virtual) continue;   // vbases: complete-object site
				DataDefCLASS *b = ocls->bases[bi].base;
				if (b->has_user_ctor)
					prologue.push_back(base_call_at(b, ocls->base_offset_of(b),
						b->name + "__" + b->name));
			}
```
In **`class_ctor_call`** (3579), after the early returns and before resolving the ctor, prepend vbase construction to the returned block. Since `class_ctor_call` returns one node, wrap vbase stmts + the ctor call in an N_BLOCK when `cdd` has vbases:
```cpp
	// (after computing `call` = the N_EXPR(ctor call) near the end, before `return call;`)
	std::vector<DataDefCLASS *> probe; std::set<DataDefCLASS *> pseen;
	cdd->collect_vbases(probe, pseen);
	if (!probe.empty()) {
		std::vector<node_t> stmts;
		vbase_ctor_stmts(node1(N_ADDR, id(v->name.c_str(), origin), origin), cdd, stmts, origin);
		node_t blk = list();
		for (node_t s : stmts) append(blk, s);
		append(blk, call);            // the ctor call AFTER the vbases
		return node2(N_BLOCK, list(), blk, origin);
	}
	return call;
```
(Also apply the same `vbase_ctor_stmts` prepend in the `new` path before the ctor/vptr-init at ~4573-4596, using the new'd temp pointer as `obj_addr` — `id(tmp)` directly, no `&`.)

- [ ] **Step 4: Run — construction order now correct (destruction still doubled until Task 2)**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_vbase_ctor.mad )`
Expected: `T+L+R+D+` for the construction prefix; destruction still `...D-R-T-L-T-` (fixed in Task 2). Do not add to the pass set yet.

- [ ] **Step 5: Commit**

```bash
git add src/cir_builder.cpp src/cir_builder.h tests/test_mi_vbase_ctor.mad tests/test_mi_vbase_ctor.expect
git commit -m "feat(cir): construct virtual bases once at the complete-object site (S4 task 1)"
```

---

## Task 2: Destroy virtual bases once via a complete-object dtor

**Files:** Modify `src/cir_builder.cpp` (`synth_dtor_def` ~3419; new `synth_complete_dtor_def`; cleanup-attribute dtor symbol; Pass 1.6 emission).

- [ ] **Step 1: (test_mi_vbase_ctor.mad is the test)** It fully passes only once destruction is fixed.

- [ ] **Step 2: Run to confirm destruction still doubled**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_vbase_ctor.mad )`
Expected: construction `T+L+R+D+` (Task 1) but destruction `D-R-T-L-T-`.

- [ ] **Step 3: Make `_dtor` the base dtor; add `_dtor_complete`; retarget cleanup**

In `synth_dtor_def` (and the user-dtor epilogue base loop ~7126-7134), **skip virtual bases** (`if (cdd->bases[bi].is_virtual) continue;`) so `Cls___dtor` destroys only members + non-virtual bases.

Add `class_complete_dtor_symbol`:
```cpp
std::string CirBuilder::class_complete_dtor_symbol(DataDefCLASS *cdd)
{
	std::vector<DataDefCLASS *> vbs; std::set<DataDefCLASS *> seen;
	cdd->collect_vbases(vbs, seen);
	return vbs.empty() ? class_dtor_symbol(cdd) : (class_dtor_symbol(cdd) + "_complete");
}
```
Add `synth_complete_dtor_def(cdd)` — emit `void Cls___dtor_complete(struct Cls *__this){ Cls___dtor(__this); <vbase dtors, reverse>; }` (mirror `synth_dtor_def`'s function shell; body = the base-dtor call then `vbase_dtor_stmts` which walks `collect_vbases` in REVERSE, each offset-adjusted, calling `class_dtor_symbol(vb)`). Add `vbase_dtor_stmts` symmetric to `vbase_ctor_stmts` (reverse order, `class_dtor_symbol`).

Emit `_dtor_complete` in **Pass 1.6** (next to `synth_dtor_def`, search `synth_dtor` in the pass loop) for every class with vbases (deduped).

Point the **cleanup attribute** at the complete dtor: wherever `obj_storage_decl`/the class stack-var cleanup uses `class_dtor_symbol(cdd)`, use `class_complete_dtor_symbol(cdd)` instead. Same for the `delete`/`new`-failure dtor call paths that destroy a complete object.

- [ ] **Step 4: Run to verify full pass + regression + SMAUG**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_vbase_ctor.mad )` → `T+L+R+D+D-R-L-T-`.
Run: `( ulimit -t 700; timeout 800 bash scripts/run_tests.sh > tmp/s4t2.log 2>&1 ); tail -1 tmp/s4t2.log` → `463 passed` (462 + test_mi_vbase_ctor), same known-6 + flaky. Single inheritance + non-virtual MI unchanged (no vbases → `_dtor_complete` not emitted, cleanup uses `_dtor`).
Run the SMAUG soak (port 4000) → boots.

- [ ] **Step 5: Commit**

```bash
git add src/cir_builder.cpp src/cir_builder.h tests/test_mi_vbase_ctor.mad
git commit -m "feat(cir): destroy virtual bases once via complete-object dtor (S4 task 2)"
```

---

## Task 3: Full S4 gate

- [ ] **Step 1: Build clean.** `( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )` → `0`.
- [ ] **Step 2: Unit tests green** (`test_class_layout` unchanged).
- [ ] **Step 3: Integration** `463 passed` (462 + test_mi_vbase_ctor), same known-6 + flaky; single-inheritance + non-virtual-MI ctor/dtor byte-identical.
- [ ] **Step 4: SMAUG soaks** (port 4000) → ready, no fatals (touches ctor/dtor + cleanup for all classes; vbase-free classes must be unchanged).
- [ ] **Step 5: Gate unchanged** (`check-no-std-hardcoding.sh` → 468) + `git push`.

---

## Self-review notes (author)
- **Spec coverage:** S4 = §8 vbase construct/destruct ordering (once, correct order). §6 construction-vtables + runtime `vbase_offset` are explicitly DEFERRED (no consumer) and logged as follow-ons — not silently dropped.
- **Regression discipline:** classes with NO vbases are untouched — `collect_vbases` empty → no site vbase ctor stmts, ctor/dtor loops behave as before (the `is_virtual` skip is a no-op), `_dtor_complete` not emitted, cleanup uses `class_dtor_symbol`. So single inheritance + non-virtual MI (incl. all of SMAUG) is byte-identical. Tasks 1/2 re-run the 462 baseline; Task 3 soaks SMAUG.
- **Type/name consistency:** `vbase_ctor_stmts`/`vbase_dtor_stmts(obj_addr, cdd, out, o)`, `synth_complete_dtor_def(cdd)`, `class_complete_dtor_symbol(cdd)` (= `class_dtor_symbol(cdd)+"_complete"` iff vbases). The vbase ctor symbol is `vb->name+"__"+vb->name`; the vbase dtor symbol is `class_dtor_symbol(vb)` — consistent with the existing schemes.
- **Flagged execution-time items:** the exact cleanup-attribute call site(s) that pass `class_dtor_symbol(cdd)` (confirm via grep `class_dtor_symbol` + `cleanup(`) and the `new`-path ctor injection point — confirm against the file when implementing (anchors given). The `synth_complete_dtor_def` function shell mirrors `synth_dtor_def` (3402-3466).
- **Known limitation carried forward:** vbase ctor/dtor are skipped for trivial (no user ctor/dtor) vbases — matching the existing `has_user_ctor`/`class_needs_dtor` gating; vptr-for-vbase (the vbase's own vptr in a vbase-only class) remains the deferred construction-vtable work.
