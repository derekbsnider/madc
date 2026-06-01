# CIR-vs-asmjit parity recovery — root-cause worklist (2026-06-01)

Measured with `scripts/run_gcc_testsuite.py --root /workspace/gcc/gcc/testsuite`
(1685 gcc.c-torture/execute tests, same runner for both backends):

| backend | passed | % |
|---|---|---|
| **master (asmjit)** | 1645 | **97.6%** |
| **CIR (feature/cir-stdstring-claude)** | 1384 | 82.1% |

**Recovery target = 265 tests** asmjit passes but CIR fails (`docs/parity/cir-vs-asmjit-regressions.txt`):
4 compile-fail (VLA/varargs) + 260 runtime mislowering + 1 timeout. 6 tests fail on
*both* backends (never chase); CIR is *ahead* on 4.

The 260 runtime mislowerings were triaged (5 parallel agents, each reducing representatives
vs gcc). They collapse to **~16 root-cause clusters** — fixing one cluster recovers many
tests, exactly like the pointer-to-array (+2), statement-expression (+2) fixes already landed.

## Fixable clusters (front-end: cir_builder / parser) — ~180 tests, ~15 root causes

Ranked by leverage (tests recovered per fix). All are cir_builder/parser bugs, NOT MIR/ABI floor gaps.

| # | cluster | ~tests | root cause / where | repro |
|---|---|---:|---|---|
| 1 | **struct/aggregate member-type resolution** ✅ CORE DONE (commit da4eb05) | ~35→24 fixed | FIXED: core root cause was every anonymous aggregate being named the literal `"anonymous"` → collided in the by-name dedup so the 2nd `typedef struct{}` degraded to incomplete `struct anonymous`. Fix: unique synthetic tags `__anon_N` + `is_anonymous` flag; inline-nested *named* structs recorded as top-level (C scope); forward-decl emission ordering; var_decl ptr/static/extern + fn-param paths preserve typedef alias / inline the anon body. +30 torture (1399→1429). REMAINING ~11 are adjacent clusters: aggregate-init "excess elements" (cluster 4: 20000801-3, 20010924-1, 921113-1, 921204-1), bitfield-through-union runtime (pr86492, 20050929-1), flexible/zero-len-array runtime (20051113-1), self-referential typedef (20090113-1/2/3), cond-expr type (memchr-1), deferred runtime-VLA (pr22061-1). | `tmp/m506.c`, `tmp/r_sizeofdim.c`, `tmp/r_nestedstruct.c` |
| 2 | **bitfield load/store** ✅ CORE DONE (commit 095d633) | ~30→13 fixed | FIXED at deepest layer: `member_node` now carries the recorded `:width` into c2mir's `N_MEMBER` bit-field slot (was always `N_IGNORE`), so c2mir does native mask-on-store / sign-or-zero-extend-on-load. +15 torture (1384→1399), +1 integration (testsignedbitfieldassignexpr). REMAINING 12 are ADJACENT clusters, not load/store: GCC reduced-precision bitfield ARITHMETIC (clang also fails — fork-level), unsigned-literal const-fold (`-13U%61` drops the U → see cluster 6), signed/unsigned hex-literal compare promotion (cluster 6), K&R call (cluster 9), union static-init/type-punning. | `tmp/bf.c`, `tmp/sbv.c`, `tmp/b16.c` |
| 3 | **varargs** | ~21 | (a) `va_arg(ap, struct)` emits a non-struct result type; (b) register-save-area boundary: with ≥6 GP / ≥8 SSE *named* args, `va_arg`'s initial `gp_offset`/`fp_offset` is wrong → reads register-save-area instead of overflow stack; (c) `long double` param slot. cir_builder `va_start`/`va_arg` lowering. | `tmp/931004-2.c`, `tmp/vatest3.c`, `tmp/va.c` |
| 4 | **aggregate initializer nesting** | ~17 | nested-brace / GNU `field:`-designator initializers flattened into one positional scalar list ("excess elements in scalar initializer" at inner brace); `static char[N]="lit"` drops the string data (auto arrays OK); array compound literals `(T[]){...}` not sized from initializer. cir_builder `translate_init`/N_LIST + static-initializer path. | `tmp/struct-ret-1`, `tmp/r_clit.c`, `tmp/stat.c` |
| 5 | **nested functions (GNU)** | ~10 | local function definitions + access to enclosing locals ("called object is not a function" / "undeclared identifier"). Needs hoisting + static-chain/trampoline lowering in cir_builder. | `tmp/r_nest.c` |
| 6 | **integer promotion / extension** | ~10 | usual-arithmetic-conversion doesn't promote signed→unsigned/wider before compare/xor; unsigned→float sign-extends instead of zero-extends; u64→u32 narrowing not truncated; unary `~0U` const-folds as signed. cir_builder `infer_numeric_type` / cast lowering (operator self-determination rules). | `tmp/r_ucmp.c`, `tmp/r_conv.c`, `tmp/sh4.c` |
| 7 | **`__builtin_*` mislowered** | ~10 | `__builtin_{add,mul}_overflow[_p]` overflow flag wrong (ignores destination width); `copysign`/`signbit`/`-0.0` sign bit lost; `__builtin_constant_p` not folded; libm `double`-return fns (pow/floor) read as int/long (embedded-header return-type bug). cir_builder builtin table + `include/madc/math.h`. | `tmp/ovf.c`, `tmp/lm.c` |
| 8 | **`_Complex` pass/return ABI** | ~7 | scalar complex arith works; passing/returning `_Complex` by value crashes (SIGSEGV) + packed-member/array/global-init gaps. cir_builder complex arg/return ABI (fork has native `_Complex`). | `tmp/r_cplx.c` |
| 9 | **K&R / untyped functions** | ~5 | K&R-defined fn called with more args than listed, and implicit-int fn `return;` rejected by c2mir strict check. parser K&R function-type handling. | `tmp/knr.c` |
| 10 | **static-local / static-array initializer dropped** | ~5 | `static int u=11;` reads 0; static arrays zeroed. cir_builder static-storage initializer emission. | `tmp/r_static.c` |
| 11 | **`__attribute__((aligned(N)))`** | ~3 | ignored on struct/member → wrong size/offset. parser parses-and-drops; layout never applies it. | `tmp/r_align.c` |
| 12 | **`__attribute__((alias))`** | ~3 | not lowered to a symbol alias → "import of undefined item". cir_builder attribute + MIR symbol emission. | `tmp/alias-2` |
| 13 | **void/comma-expr ternary type** | ~3 | `cond ? (void)0 : (printf(...),abort())` — a void/comma branch not typed `void` → "incompatible types in cond-expression". cir_builder ternary type-merge / comma-result type. | strlen-2 |
| 14 | **void\* / incomplete-type ptr arithmetic** | ~2 | `void* + int` rejected. parser/cir_builder pointer-arith type check. | pr17133 |
| 15 | **setjmp/longjmp**, misc singletons | ~10 | `__builtin_longjmp`+alloca; computed-goto label scope; union-by-value aliased members; `case` in dead `if(0){}`; block-scope `extern` resolves to local; float→int saturate-vs-wrap. | various |

## Deferred — needs care (NOT floor gaps, but regress SMAUG / non-trivial)

- **K&R / unprototyped functions (~5)** — ✅ DONE 2026-06-01 (commit `ea61546`, recovered 5/5, torture 1521→1527). Emit `(void)` only for `is_void_params`; bare K&R `()` stays unprototyped, consistent across func_def/func_proto/fnptr_func_node; + typed-zero for bare `return;`. SMAUG boots clean (coordinator-verified, no `get_color` crash), real arg-mismatch still errors. **ROOT-CAUSE CAVEAT:** the first attempt crashed SMAUG `get_color`; the re-fix's subagent theorized that was the fork missing `c40ed46`, but the TIMELINE DISPROVES it (016eb2f@16:20 ran after the fork was already at 1fdf44d@15:52 ⊇ c40ed46@12:44). The real reason the identical change now works is most likely the FAM (`ef0f1e8`) / self-ref-typedef (`137d230`) emission commits that landed in between — NOT definitively pinned. RISK: the get_color path may be fragile; **any future change near function-prototype emission MUST re-run the SMAUG soak** (grep the literal "ready at" line). Original history below (kept for the analysis).
- (historical) K&R first attempt — ATTEMPTED 2026-06-01, REVERTED (patch `tmp/knr-cluster-016eb2f.patch`). Emitting a bare `()` zero-param function as *unprototyped* (instead of `(void)`) makes the 5 torture tests pass BUT regresses SMAUG (`get_color` SIGSEGV [JIT], exit 139) — SMAUG's C89 code uses empty-parens `int foo()` ubiquitously for genuine zero-param functions, and the unprototyped emission corrupts their codegen. **Layer CONFIRMED (2026-06-01 probe): it is NOT c2mir — stock `c2m` handles unprototyped functions correctly in all three cases (0-param def called with 0 args; 0-param def called with extra args `f(1,2,3)`; unprototyped decl + mismatched def — all rc=0).** So the regression is a madc EMISSION-CONSISTENCY bug: flipping `func_def`/`func_proto` to bare `()` introduced a type mismatch c2mir choked on for SMAUG's patterns — most likely a function emitted unprototyped `()` while a typed **function pointer** to it (SMAUG's `DO_FUN`/`SPEC_FUN`) or a forward declaration still carries a concrete signature → indirect-call type mismatch → MIR miscompile → `get_color` SIGSEGV. The `translate_return` half (typed-zero for bare `return;`, recovers 920728-1) is SMAUG-safe and separable. NEXT (madc-side, no fork): make unprototyped emission CONSISTENT across decl/def/fn-ptr-target for the same function — or scope unprototyped to genuine no-prototype DECLARATIONS and keep `(void)` for empty-parens DEFINITIONS that are referenced by typed pointers. Re-verify with a coordinator-run SMAUG soak (the subagent's soak FALSELY reported a clean boot — always re-run it + grep the literal "ready at" line).

## Deferred clusters (floor gaps — NOT quick front-end fixes) — ~45 tests

| cluster | ~tests | why deferred |
|---|---:|---|
| **SIMD `vector_size`** | ~30 | MIR floor gap (locals i64/f/d/ld only). Tier-3 raise-MIR, separately roadmapped. asmjit passed these via direct x86. |
| inline `asm` | ~5 | c2mir has no inline asm; lower/skip. |
| wide-string / `wchar_t` | ~3 | c2mir floor gap (`L"..."`). |
| `__int128` | ~3 | likely MIR width gap — verify against fork. |
| VLA (`sizeof`, in-struct, in-loop) | ~4 | Tier-3 MIR; user-deferred with SIMD. |

## Effort read

~260 runtime fails = **~16 root causes**, not 260 bugs. The top 4 fixable clusters
(struct-member-resolution, bitfield, varargs, aggregate-init) alone ≈ **~103 tests**.
Fixing the ~15 fixable clusters recovers ~180 tests → CIR ~82% → ~93-95%, at/near asmjit's
97.6% modulo the ~45 deferred floor-gap tests (SIMD/VLA/asm/wchar/int128). That is a bounded
campaign of cluster-fixes (each gcc-compared + gate-verified + SMAUG-soaked), **not** weeks of
per-test whack-a-mole. Reducers for every cluster are in `tmp/` (regenerate via the batch lists
`tmp/rtbatch_0[0-4]`).

Method per cluster: reduce → gcc/asmjit-diff → fix at deepest layer (parser type or cir_builder
translate_*) → `make -C src fulltest` (no regressions) → re-run torture for the cluster → commit.

## Progress update 2026-06-01 (session 2)

CIR torture **1382 -> 1541 (91.5%)**, integration 450, SMAUG boots throughout. This
session added: label+block-extern (+2, 5ca8492) and the **UNION cluster COMPLETE
(+11)** — union members now overlap (N_UNION at definition, type-spec reference,
typedef'd-anonymous, function-return, var-decl, and extern sites; previously all
hardcoded N_STRUCT). Recovered pr86492, pr82524, 20180131-1, bitfld-6, bitfld-7,
+6 more. Also: legacy cir_translate path removed (one backend), test_cir repointed
at the live CirBuilder, `make test` capped (ulimit+timeout), `scripts/resume.sh`
preflight, rule `no-parallel-implementations`.

**Remaining clusters are now deeper / floor-level (no more cheap coherent wins):**
- reduced-precision bitfield ARITHMETIC (bitfld-3/5) — clang also fails; fork-level.
- `scalar_storage_order` (20230630-2/4, pr87623) — unimplemented byte-swap feature.
- out-of-range float->int conversion (20031003-1) — MIR conversion semantics.
- `__builtin_setjmp`/`__builtin_longjmp` + `__builtin_alloca` (pr64242, built-in-setjmp)
  — GCC stack intrinsics, c2mir/MIR floor.
- ~45 deferred floor gaps (SIMD ~30 / VLA / inline asm / wchar / __int128 / aligned>16).
Pick these as deliberate, individually-scoped efforts — each needs real
investigation (not a one-liner) and most touch the MIR fork.

## Refined triage of remaining fails 2026-06-01 (full sweep, not sampling)

Bucketed ALL 75 "exit 1" torture fails by their REAL first error (tmp/exit1.list).
KEY: the big raw buckets are ~70% SIMD floor-gap contamination — filter on
`vector_size` before calling anything cheap.

- "excess elements in scalar initializer" raw 21 = **18 SIMD (floor)** + 4 genuine
  aggregate-init (pr109938, pr109986, pr98366, strlen-4).
- "subscripted value is neither array nor pointer" raw ~25 = **17 SIMD (floor)** +
  8 other, of which widechar-3 (wchar floor) + pr22061-1 (VLA floor) are also floor.
- ~69 of 75 "exit 1" are COMPILE/CHECK errors (only 6 true runtime value mismatch).

**Genuinely-cheap front-end remainder (~15-20 tests, NOT 40):**
- aggregate-init / array compound-literal `(T[]){...}` sizing + designated/bitfield
  init: pr98366 (`(S[]){{.b=3,..}}`), pr109938, pr109986, strlen-4 (multidim
  typedef'd array-of-ptr declarator). ~4-7 tests.
- statement-expression last-statement-as-value, NESTED form `*({ ({...}); })`:
  20000917-1, 20001203-2, 20020206-1 (3). N_STMTEXPR exists; last-stmt extraction
  incomplete when the last stmt is itself a stmt-expr/expr-stmt.
- libc auto-declare: abort/exit/sprintf "undeclared identifier" (3) — embedded-header
  / dlsym-declare gap.
- braces-around-scalar (3, likely aggregate-init family); a few singletons
  (cond-expr types, incomplete return type).

**Floor (defer, ~floor count grew with this sweep):** SIMD vector_size ~35+ (the
dominant remaining bucket), inline asm (4, "undeclared identifier asm"),
aligned>16 (4 "unsupported alignmnent"), wchar/`L"..."` (`__wliteral__`), VLA,
__builtin_setjmp/longjmp+alloca, scalar_storage_order, float->int saturate.

CORRECTION to the earlier "cheap wins ran out": there are ~15-20 genuinely cheap
front-end tests left (aggregate-init + stmt-expr + libc-declare the best targets),
but most raw error buckets are SIMD-diluted, so the cheap YIELD is modest and each
remaining cheap fix is a real declarator/initializer change, not a one-liner.

## Cleanup clusters cleared 2026-06-01 (session 2 cont'd) — 1538 -> 1547

Cheap front-end clusters done (each gcc-diffed, failset-verified zero-regression,
SMAUG-soaked, pushed):
- union tag-kind (def/ref/typedef/return/var/extern) +11 [14fc16c/187b135/f9d7566]
- nested statement-expression last-value `({ ({...}); })` +3 [069fb8b]
- top-level function used as a VALUE (address-of / fn-ptr decay) +3 [9ac7a1b]

REMAINING cheap front-end (the one left, more involved — good fresh-session item):
- aggregate-init / array compound-literal `(T[]){...}` sizing + designated/bitfield
  init: pr98366, pr109938, pr109986, strlen-4 (~4-7). Needs declarator/initializer
  work (multidim typedef'd array-of-ptr; compound-literal array sizing from init).

Everything else is FLOOR/feature-level (fresh session, likely fork work): SIMD
vector_size (~35, dominant), inline asm, aligned>16, wchar/L"...", VLA,
__builtin_setjmp/longjmp+alloca, scalar_storage_order, float->int saturate,
reduced-precision bitfield arithmetic (bitfld-3/5).
