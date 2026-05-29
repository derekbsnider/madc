# CIR Fidelity Test Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a `cir_node → C11` renderer (`--emit=c11`) plus a gcc-fidelity gate and a `c2m -d` tree differential, so the 193 CIR failures (esp. the 96 `c2mir_rejected`) become localized, mechanical diffs.

**Architecture:** A new `node_t → C` pretty-printer walks the MC11-IR (`cir_node` IS-A c2mir `node_t`) and emits C, mirroring the existing `cir_dump_node` walker in `src/cir_builder.cpp`. A new `madc_cir_emit()` entry builds the tree (no execute) and calls the printer. Shell scripts drive the gcc-`-S` fidelity diff and the existing `--dump-cir`-vs-`c2m -d` differential. The suite is diagnostic tooling, tested via golden plain-C reducers (a pretty-printer CLI is naturally tested through its output, not doctest).

**Tech Stack:** C++11, c2mir node API (`c2mir_node_op`, `c2mir_node_code_name`, node codes `N_*` from `c2mir/c2mir_node_code.h`), bash, gcc, `c2m`.

**Reference shapes (verified):**
- `struct node { node_code_t code; unsigned uid; void *attr; DLIST_LINK op_link; union { c2mir_str_t s; c2mir_long l; c2mir_double d; ...; DLIST(node_t) ops; } u; };` (`/workspace/mir/c2mir/c2mir_node.h`)
- Operand access: `node_t c2mir_node_op(node_t n, int i)` returns the i-th operand, NULL past end. Interior nodes have `n->code > N_ID`; leaves (`<= N_ID`) carry a scalar in `n->u`.
- Template walker: `cir_dump_node()` at `src/cir_builder.cpp:1587`.
- CIR execute entry: `madc_cir_execute()` at `src/madc_cir.cpp:1615`; tree built by `CirBuilder::translate_module(prog)` (`src/cir_builder.cpp:1377`).
- madc.cpp: `--emit-c` stub errors at `src/madc.cpp:424`; `--backend=` block at `src/madc.cpp:391-411`; CIR invoked at `src/madc.cpp:544`.

---

## File Structure

- **Create `src/cir_emit_c.h`** — public decl + `enum CirEmitLang { celC11, celMC11 }` and `void cir_emit_c(FILE *f, node_t tree, CirEmitLang lang);`
- **Create `src/cir_emit_c.cpp`** — the `node_t → C` pretty-printer (the new subsystem).
- **Modify `src/madc_cir.cpp`** — add `int madc_cir_emit(Program *prog, const char *source_name, FILE *out, CirEmitLang lang);` (build tree via `translate_module`, call `cir_emit_c`, no compile/run).
- **Modify `src/madc_cir.h`** — declare `madc_cir_emit` and include the lang enum.
- **Modify `src/madc.cpp`** — replace the `--emit-c` stub with `--emit=c11|mc11`; delete the `--backend=` block.
- **Modify `src/Makefile`** — add `cir_emit_c.o` to `CORE_OFILES`.
- **Create `scripts/cir_fidelity.sh`** — gcc-`-S` fidelity gate + `--dump-cir`-vs-`c2m -d` differential, single test or `--all`.
- **Create `tests/fidelity/`** — golden plain-C reducers (`*.c`) used by the gate.

---

## Task 1: `--emit=c11` skeleton renderer + remove `--backend`

**Files:**
- Create: `src/cir_emit_c.h`, `src/cir_emit_c.cpp`
- Modify: `src/madc_cir.h`, `src/madc_cir.cpp`, `src/madc.cpp`, `src/Makefile`
- Test: `tests/fidelity/ret0.c`, `tests/fidelity/ret42.c` (golden inputs)

- [ ] **Step 1: Write the failing test (golden reducers + a shell check)**

Create `tests/fidelity/ret0.c`:
```c
int main(void) { return 0; }
```
Create `tests/fidelity/ret42.c`:
```c
int main(void) { return 42; }
```
Create `tmp/fid_test.sh` (scratch, per scratch-files rule):
```bash
#!/bin/bash
set -u
bin/madc --emit=c11 tests/fidelity/ret42.c > tmp/ret42.emitted.c 2>tmp/ret42.err
grep -q "int main" tmp/ret42.emitted.c || { echo "FAIL: no 'int main'"; cat tmp/ret42.err; exit 1; }
grep -q "return 42" tmp/ret42.emitted.c || { echo "FAIL: no 'return 42'"; exit 1; }
gcc -x c -o tmp/ret42.bin tmp/ret42.emitted.c || { echo "FAIL: emitted C did not compile"; exit 1; }
tmp/ret42.bin; rc=$?
[ "$rc" -eq 42 ] || { echo "FAIL: emitted binary exit $rc != 42"; exit 1; }
echo "PASS"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bash tmp/fid_test.sh`
Expected: FAIL — `--emit=c11` is not yet a flag (`--emit-c` currently errors; `--emit=c11` is unknown), so no `int main` in output.

- [ ] **Step 3: Create `src/cir_emit_c.h`**

```cpp
#ifndef __CIR_EMIT_C_H
#define __CIR_EMIT_C_H 1

extern "C" {
#include "c2mir/c2mir_node.h"
}
#include <cstdio>

// Output language target. Shares its meaning with --std=/--emit=:
// celC11 strips madc metadata; celMC11 adds `madc`-namespaced pragmas.
enum CirEmitLang { celC11 = 0, celMC11 = 1 };

// Render a cir_node tree (which IS-A c2mir node_t) to C source on `f`.
void cir_emit_c(FILE *f, node_t tree, CirEmitLang lang);

#endif // __CIR_EMIT_C_H
```

- [ ] **Step 4: Create `src/cir_emit_c.cpp` (skeleton handling N_MODULE/N_FUNC_DEF/N_BLOCK/N_RETURN/type-specs/N_ID/N_DECL/N_FUNC/N_LIST/N_I)**

```cpp
#include "cir_emit_c.h"
#include "cir_node.h"      // CIR_NODE, struct node access
#include <cstring>

extern "C" {
#include "c2mir/c2mir_api.h"   // c2mir_node_op, c2mir_node_code_name
}

namespace {

void emit(FILE *f, node_t n, CirEmitLang lang, int depth);

// i-th operand or NULL
inline node_t op(node_t n, int i) { return c2mir_node_op(n, i); }

// Emit a comma/space-separated operand list starting at index `from`.
void emit_children(FILE *f, node_t n, CirEmitLang lang, int depth,
                   int from, const char *sep)
{
	for (int i = from; ; i++) {
		node_t c = op(n, i);
		if (!c) break;
		if (i > from) fputs(sep, f);
		emit(f, c, lang, depth);
	}
}

void emit(FILE *f, node_t n, CirEmitLang lang, int depth)
{
	if (!n) return;
	switch (n->code) {
	case N_MODULE:
		// MODULE -> a single N_LIST of top-level declarations/defs
		emit_children(f, op(n, 0) ? op(n, 0) : n, lang, depth, 0, "\n");
		fputc('\n', f);
		break;
	case N_LIST:
		emit_children(f, n, lang, depth, 0, " ");
		break;
	case N_FUNC_DEF: {
		// ops: [specs(N_LIST)] [declarator(N_DECL)] [decl-list(N_LIST)] [body(N_BLOCK)]
		emit(f, op(n, 0), lang, depth);          // return-type specifiers
		fputc(' ', f);
		emit(f, op(n, 1), lang, depth);          // declarator (name + N_FUNC params)
		fputc(' ', f);
		emit(f, op(n, 3), lang, depth);          // body block
		break;
	}
	case N_BLOCK:
		fputs("{\n", f);
		emit_children(f, n, lang, depth + 1, 0, "\n");
		fputs("\n}", f);
		break;
	case N_RETURN:
		fputs("return", f);
		if (op(n, 0) && op(n, 0)->code != N_IGNORE) { fputc(' ', f); emit(f, op(n, 0), lang, depth); }
		fputc(';', f);
		break;
	case N_DECL:
		// ops: [declarator-id or N_FUNC] ...; minimal: name then nested
		emit_children(f, n, lang, depth, 0, "");
		break;
	case N_FUNC:
		// ops: [param-list]; minimal void
		fputs("(void)", f);
		break;
	case N_ID:   fputs(n->u.s.s ? n->u.s.s : "", f); break;
	case N_I:
	case N_L:    fprintf(f, "%lld", (long long)n->u.l); break;
	case N_VOID: fputs("void", f); break;
	case N_CHAR: fputs("char", f); break;
	case N_INT:  fputs("int", f); break;
	case N_LONG: fputs("long", f); break;
	case N_IGNORE: break;
	default:
		// Unhandled node kind: emit a visible marker so the fidelity diff
		// localizes exactly which construct the renderer is missing.
		fprintf(f, "/*<unhandled %s>*/",
		        c2mir_node_code_name((c2mir_node_code_t)n->code));
		break;
	}
}

} // namespace

void cir_emit_c(FILE *f, node_t tree, CirEmitLang lang)
{
	emit(f, tree, lang, 0);
}
```

NOTE: the exact operand indices for `N_FUNC_DEF`/`N_DECL` must match what
`CirBuilder` builds — verify against `cir_builder.cpp` (`translate_func` /
`build_func_def`) while implementing; adjust indices if the skeleton's
`int main(void){...}` output is malformed. The `default` marker is intentional:
it turns "missing construct" into a localized comment in the emitted C.

- [ ] **Step 5: Add `madc_cir_emit` to `src/madc_cir.cpp` (after `madc_cir_execute`)**

```cpp
#include "cir_emit_c.h"

int madc_cir_emit(Program *prog, const char *source_name, FILE *out,
                  CirEmitLang lang)
{
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	c2m_ctx_t c2m = cir_init(ctx, /*dump_checked=*/false);
	if (!c2m) { fprintf(stderr, "madc_cir_emit: cir_init failed\n"); MIR_finish(ctx); return -1; }

	CirBuilder builder(c2m);
	node_t tree = builder.translate_module(prog);
	if (!tree) { fprintf(stderr, "madc_cir_emit: tree build failed\n"); cir_finish(c2m); c2mir_finish(ctx); MIR_finish(ctx); return -1; }

	cir_emit_c(out, tree, lang);

	cir_finish(c2m);
	c2mir_finish(ctx);
	MIR_finish(ctx);
	return 0;
}
```
(`source_name` is accepted for signature symmetry with `madc_cir_execute`; not
yet used. Mark `(void)source_name;` to silence the warning.)

- [ ] **Step 6: Declare it in `src/madc_cir.h`**

```cpp
#include "cir_emit_c.h"   // CirEmitLang
int madc_cir_emit(Program *prog, const char *source_name, FILE *out,
                  CirEmitLang lang);
```

- [ ] **Step 7: Wire `--emit=c11|mc11` and delete `--backend` in `src/madc.cpp`**

Delete the entire `--backend=` block (`src/madc.cpp:391-411`).

Replace the `--emit-c` stub block (`src/madc.cpp:424-...`) with:
```cpp
		} else if (strncmp(argv[i], "--emit=", 7) == 0) {
			const char *lang = argv[i] + 7;
			if (strcmp(lang, "c11") == 0)        emit_lang = celC11;
			else if (strcmp(lang, "mc11") == 0)  emit_lang = celMC11;
			else { std::cerr << "Unknown --emit target: " << lang
			                 << " (c11|mc11)" << std::endl; return 1; }
			do_emit = true;
			filearg = i + 1;
```
Add near the other flag locals (around `src/madc.cpp:344`):
```cpp
	bool do_emit = false;
	CirEmitLang emit_lang = celC11;
```
Add the dispatch just before the `madc_cir_execute` call (`src/madc.cpp:544`):
```cpp
	if (do_emit)
		return madc_cir_emit(prog.get(), argv[filearg], stdout, emit_lang);
```
Add `#include "cir_emit_c.h"` to madc.cpp's includes.

- [ ] **Step 8: Add `cir_emit_c.o` to `src/Makefile` `CORE_OFILES`**

Find the `CORE_OFILES = ...` list and add `cir_emit_c.o` next to `cir_builder.o`.

- [ ] **Step 9: Build**

Run: `make -C src`
Expected: clean build (the `-MMD` deps pick up the new files).

- [ ] **Step 10: Run the test to verify it passes**

Run: `bash tmp/fid_test.sh`
Expected: `PASS` (emitted C contains `int main`, `return 42`, compiles, exits 42). If `N_FUNC_DEF`/`N_DECL` operand indices are wrong, fix per the Step 4 NOTE and re-run.

- [ ] **Step 11: Sanity — `--backend` is gone, default run still works**

Run: `bin/madc --backend=cir tests/fidelity/ret42.c; echo $?`
Expected: nonzero with "Unknown argument" (flag removed).
Run: `bin/madc tests/fidelity/ret42.c; echo $?`
Expected: `42` (bare invocation still executes via CIR).

- [ ] **Step 12: Commit**

```bash
git add src/cir_emit_c.h src/cir_emit_c.cpp src/madc_cir.cpp src/madc_cir.h src/madc.cpp src/Makefile tests/fidelity/ret0.c tests/fidelity/ret42.c
git commit -m "feat(cir): --emit=c11 renderer skeleton (node_t->C); remove vestigial --backend"
```

---

## Task 2: Fidelity gate script + grow the renderer to pass plain-C reducers

**Files:**
- Create: `scripts/cir_fidelity.sh`
- Create: `tests/fidelity/{arith,call_printf,locals,ifelse,loop}.c` (small plain-C reducers)
- Modify: `src/cir_emit_c.cpp` (add node kinds the reducers need)

- [ ] **Step 1: Write `scripts/cir_fidelity.sh` (the failing harness)**

```bash
#!/bin/bash
# cir_fidelity.sh — localize CIR renderer divergence against gcc.
# Usage: scripts/cir_fidelity.sh <file.c|file.mad>
# Emits C via madc, compiles both original and emitted with gcc -S, and
# diffs per-function label-normalized assembly. Generic; no per-test logic.
set -u
src="$1"
base=$(basename "$src" | sed 's/\.[^.]*$//')
work=tmp/fid; mkdir -p "$work"
emitted="$work/$base.emitted.c"

bin/madc --emit=c11 "$src" > "$emitted" 2>"$work/$base.emit.err" || {
	echo "EMIT-FAIL $base"; sed -n '1,5p' "$work/$base.emit.err"; exit 1; }

# Unhandled-node markers localize a missing construct directly.
if grep -q "<unhandled" "$emitted"; then
	echo "UNHANDLED $base:"; grep -o "<unhandled [A-Z_]*>" "$emitted" | sort | uniq -c; exit 2
fi

norm() { gcc -S -fverbose-asm -O0 -x c "$1" -o - 2>/dev/null \
	| sed -E 's/\.L[0-9]+/.L/g; /^\s*\.(file|ident|cfi|loc)/d; s/#.*$//'; }

if ! diff <(norm "$src") <(norm "$emitted") > "$work/$base.asm.diff"; then
	echo "ASM-DIVERGE $base ($(grep -c '^[<>]' "$work/$base.asm.diff") lines) -> $work/$base.asm.diff"
	exit 3
fi
echo "FIDELITY-OK $base"
```
Create `tests/fidelity/arith.c`:
```c
int main(void) { int a = 6, b = 7; return a * b - 1; }
```

- [ ] **Step 2: Run to verify it fails / localizes**

Run: `bash scripts/cir_fidelity.sh tests/fidelity/arith.c`
Expected: `UNHANDLED arith:` listing missing node kinds (e.g. `N_SPEC_DECL`, `N_MUL`, `N_SUB`, `N_INIT`) — the renderer skeleton doesn't handle locals/arithmetic yet.

- [ ] **Step 3: Extend `src/cir_emit_c.cpp` for the reducer's node kinds**

Add cases (binary operators wrap in parens to preserve precedence cheaply):
```cpp
	case N_ADD: case N_SUB: case N_MUL: case N_DIV: case N_MOD:
	case N_EQ:  case N_NE:  case N_LT:  case N_LE: case N_GT: case N_GE:
	case N_AND: case N_OR:  case N_XOR: case N_LSH: case N_RSH:
	case N_ANDAND: case N_OROR: case N_ASSIGN: {
		static const struct { int code; const char *op; } M[] = {
			{N_ADD,"+"},{N_SUB,"-"},{N_MUL,"*"},{N_DIV,"/"},{N_MOD,"%"},
			{N_EQ,"=="},{N_NE,"!="},{N_LT,"<"},{N_LE,"<="},{N_GT,">"},{N_GE,">="},
			{N_AND,"&"},{N_OR,"|"},{N_XOR,"^"},{N_LSH,"<<"},{N_RSH,">>"},
			{N_ANDAND,"&&"},{N_OROR,"||"},{N_ASSIGN,"="},{0,0}};
		const char *o = "?"; for (int k=0; M[k].op; k++) if (M[k].code==n->code) o=M[k].op;
		fputc('(', f); emit(f, op(n,0), lang, depth);
		fprintf(f, " %s ", o); emit(f, op(n,1), lang, depth); fputc(')', f);
		break;
	}
	case N_SPEC_DECL:  // [specs(N_LIST)] [declarator] [initializer]
		emit(f, op(n,0), lang, depth); fputc(' ', f);
		emit(f, op(n,1), lang, depth);
		if (op(n,2) && op(n,2)->code != N_IGNORE) { fputs(" = ", f); emit(f, op(n,2), lang, depth); }
		fputc(';', f);
		break;
	case N_EXPR:  emit(f, op(n,0), lang, depth); fputc(';', f); break;
```
(Verify `N_SPEC_DECL` operand layout against `cir_builder.cpp` while
implementing — adjust indices if the emitted local decl is malformed.)

- [ ] **Step 4: Run to verify fidelity**

Run: `make -C src` then `bash scripts/cir_fidelity.sh tests/fidelity/arith.c`
Expected: `FIDELITY-OK arith` (emitted C produces byte-identical normalized asm to the original).

- [ ] **Step 5: Add the remaining reducers and iterate**

Create `tests/fidelity/call_printf.c`, `locals.c`, `ifelse.c`, `loop.c` (each a
minimal plain-C program exercising one construct family: function calls +
string literal, multiple locals, `if/else`, `while`/`for`). For each, run the
gate; when it reports `UNHANDLED` or `ASM-DIVERGE`, add the node kind(s)
(`N_CALL`, `N_STR`, `N_IF`, `N_WHILE`, `N_FOR`, `N_COND`, `N_CAST`, `N_DEREF`,
`N_ADDR`, `N_IND`, `N_FIELD`, etc.) to `cir_emit_c.cpp` until each reports
`FIDELITY-OK`. Commit after each construct family lands.

- [ ] **Step 6: Commit (per construct family)**

```bash
git add src/cir_emit_c.cpp scripts/cir_fidelity.sh tests/fidelity/
git commit -m "feat(cir-emit): <construct family> + fidelity reducer"
```

---

## Task 3: Tree differential leg (`--dump-cir` vs `c2m -d`)

**Files:**
- Modify: `scripts/cir_fidelity.sh` (add the tree-diff leg)

The pre-check dump already exists: `madc_cir_execute(..., dump_tree=true, ...)`
calls `c2mir_dump_tree` (c2mir's own printer) — surfaced via `--dump-cir`. So
this task is wiring + diff, no new C.

- [ ] **Step 1: Confirm the dump flags produce comparable output**

Run: `bin/madc --dump-cir tests/fidelity/arith.c 2>&1 | sed -n '1,20p'`
Run: `/workspace/mir/c2m -d tmp/fid/arith.emitted.c 2>&1 | sed -n '1,20p'`
Expected: both print an indented node tree in the same `print_node` format.
(If `--dump-cir` format differs, note it; alignment is a follow-up, not a blocker.)

- [ ] **Step 2: Add the differential leg to `scripts/cir_fidelity.sh`**

Append before the final `FIDELITY-OK`:
```bash
treediff="$work/$base.tree.diff"
diff <(bin/madc --dump-cir "$src" 2>&1 | sed -E 's/@[^ ]+:[0-9]+:[0-9]+//; s/ uid=[0-9]+//') \
     <(/workspace/mir/c2m -d "$emitted" 2>&1) > "$treediff" 2>/dev/null
if [ -s "$treediff" ]; then
	echo "TREE-DIVERGE $base ($(grep -c '^[<>]' "$treediff") lines) -> $treediff"
fi
```

- [ ] **Step 3: Run on a reducer and a known `c2mir_rejected` test**

Run: `bash scripts/cir_fidelity.sh tests/fidelity/arith.c`
Expected: `FIDELITY-OK arith` and no `TREE-DIVERGE` (or a small, explicable one).
Run: `bash scripts/cir_fidelity.sh tests/teststruct.mad`
Expected: a `TREE-DIVERGE`/`ASM-DIVERGE`/`UNHANDLED` line pointing at the failing construct (this is the suite doing its job on a real failure).

- [ ] **Step 4: Commit**

```bash
git add scripts/cir_fidelity.sh
git commit -m "feat(cir-fidelity): add --dump-cir vs c2m -d tree differential leg"
```

---

## Task 4: `make` target + corpus run over the 96 `c2mir_rejected`

**Files:**
- Modify: `scripts/cir_fidelity.sh` (add `--all` corpus mode)
- Modify: `src/Makefile` (add `cirfidelity` target)

- [ ] **Step 1: Add `--all` mode to `scripts/cir_fidelity.sh`**

```bash
if [ "${1:-}" = "--all" ]; then
	for t in tests/*.mad tests/fidelity/*.c; do
		b=$(basename "$t" | sed 's/\.[^.]*$//')
		[ "$b" = "include_helper" ] && continue
		[ -f "tests/$b.mir_skip" ] && continue
		bash "$0" "$t"
	done | sort | uniq -c | awk '{print}' 
	exit 0
fi
```
(Place this block at the top of the script, after `set -u`.)

- [ ] **Step 2: Add the `cirfidelity` make target to `src/Makefile`**

```make
cirfidelity: ../bin/madc
	cd .. && bash scripts/cir_fidelity.sh --all
```

- [ ] **Step 3: Run the corpus**

Run: `make -C src cirfidelity`
Expected: a summary of `FIDELITY-OK` / `UNHANDLED` / `ASM-DIVERGE` /
`TREE-DIVERGE` / `EMIT-FAIL` counts. The `UNHANDLED`/`TREE-DIVERGE` lines are
the prioritized worklist for the next round of renderer/builder fixes.

- [ ] **Step 4: Capture the worklist**

Run: `make -C src cirfidelity > tmp/fidelity_run.txt 2>&1; grep -hoE "<unhandled [A-Z_]*>" tmp/fid/*.emitted.c | sort | uniq -c | sort -rn`
Expected: a ranked histogram of missing node kinds across the corpus — the
direct input to the next implementation round.

- [ ] **Step 5: Commit**

```bash
git add scripts/cir_fidelity.sh src/Makefile
git commit -m "feat(cir-fidelity): --all corpus mode + make cirfidelity target"
```

---

## Self-Review

**Spec coverage:**
- Renderer `--emit=c11|mc11` + `Lang` enum + strip-to-c11 → Task 1 (skeleton) + Task 2 (growth). `mc11` metadata is minimal-by-design (deferred per spec "Open"); `--emit=mc11` accepted, emits same as c11 until metadata schema lands. ✓
- gcc-`-S -fverbose-asm` fidelity gate, per-fn label-normalized → Task 2 (`norm()` + diff). ✓
- `--dump-cir` vs `c2m -d` differential, reusing c2mir printer → Task 3. ✓
- Harness + corpus over the 96 → Task 4. ✓
- Remove vestigial `--backend` → Task 1 Step 7. ✓
- Non-plain-C inputs skip the gcc leg → the gate reports `ASM-DIVERGE`/`UNHANDLED`/`EMIT-FAIL` rather than asserting identity; the C++/madc-lowered cases naturally surface as divergences to triage (acceptable; the spec scopes the gcc leg as "sharpest for plain C," not universal).

**Placeholder scan:** Task 2 Step 5 intentionally describes an iterative growth loop rather than enumerating all ~130 node codes — this is the suite's purpose (the gate names the next missing node), not a placeholder. Operand-index NOTES in Tasks 1–2 are real verification instructions, not TBDs.

**Type consistency:** `CirEmitLang { celC11, celMC11 }`, `cir_emit_c(FILE*, node_t, CirEmitLang)`, `madc_cir_emit(Program*, const char*, FILE*, CirEmitLang)`, `c2mir_node_op(node_t,int)`, `do_emit`/`emit_lang` locals — consistent across Tasks 1–4. ✓
