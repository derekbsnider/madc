# gcc.c-torture failset classification — the standard-C vs gcc-only dividing line

**UPDATE 2026-08-12 (task #41 window CLOSED — baseline restored):** full
sweep **1614/1/9/0/61** — byte-identical to the 2026-07-23 baseline
(@9a48a7fc); the failset is again exactly the classified 10-name
class-(b) set. All 5 window fails were **class-(a) standard C** and are
FIXED on `feature/torture-window-t41-claude` (533aeae5 long-double
struct alignment 16; ac1c583f `__builtin_classify_type` as a real
parser builtin; 2bb2dda1 anonymous-aggregate inline emission after
nested-type class promotion; 71d83a21 nested-brace aggregate recursion
in designated inits) — each with a gcc+clang-oracled reducer in
`tests/` (testldblalign, testclassifytype, testanonnested,
testnesteddesig). The 2026-08-11 paragraph's unprototyped-call
hypothesis is DISPROVEN: the window's own 2026-07-27 type-correctness
work (114b13a8 long double = real 16-byte type; 6fec105d nested types
promote the enclosing struct) unmasked three latent defects, and
8f8f4009's loud-shape gate exposed the fourth. **The promote gate
(zero class-(a) outstanding) is MET again at this branch.**

**UPDATE 2026-08-11 (MIR-subtree migration validation sweep):** full
sweep **1609/2/13/0/61** — the classified 10-name class-(b) failset is
intact, plus **5 NEW fails**: `20000717-4` (exit 1), `20040709-1/-2/-3`
(runtime diagnostic), `pr39339` (compile diagnostic). **Attribution:
pre-existing, NOT the migration** — all 5 fail identically on the
pre-migration v0.76.0 build (@16e04001, standalone `/workspace/mir`);
the regression window is @9a48a7fc (last sweep, 2026-07-23) →
@16e04001, in which torture never ran. v0.76.0 was promoted to master
carrying these. Unverified hypothesis for the follow-up: the
unprototyped-call proto change (fork e2c0ae95+731c2234, session #74) —
the failing class is K&R/struct-passing. Classification pending; if any
of the 5 is class-(a), the promote gate is retroactively unmet at HEAD
and blocks the next promote until fixed.

**Date:** 2026-06-11. **Input failset:** `tmp/failset_lsq.txt` (88 tests: 31
compile-fail, 56 runtime-fail, 1 timeout) at develop `8dfa827` (binary
baseline 1567/1685 = 93.0%).
**Status: SIGNED OFF (user, 2026-06-11).** Gate wording + skip manifest
agreed; VLA-in-struct confirmed (c) (clang: "'variable length array in
structure' extension will never be supported" — verified live); `string`
ruled NOT a madc builtin type (see work item 6). Gate edits landed in
`docs/adr/0001-cir-c2mir-backend.md`, `.claude/rules/branching.md`,
`docs/rules/branching.md`, ROADMAP Track 1.3.

**UPDATE 2026-07-23 (task #80, @9a48a7fc):** pr22061-1 FIXED — VLA
parameter bounds now lower to a flat scalar pointer with entry-time
dim capture (pure madc-side Tier-1; no fork raise — stock c2m rejects
ALL VLAs, params included, so c2mir was never going to see one).
Verified through `scripts/run_gcc_testsuite.py` at HEAD. The failset
drops 11 → **10**, all genuine class-(b) GNU-extension roadmap items
(SIMD >16B: simd-1/2, pr23135, pr92904, pr46309; aligned>16/misalign:
20010904-1/2, misalign; pr23324; pr122000). pr122000 STAYS: batch 1's
#65 fixed its float→int saturation half, but the test also requires
the `__sync_add_and_fetch` atomic builtin (undefined MIR import) — a
separate class-(b) builtin gap. Notes: (1) the 2026-07-22 paragraph
below loosely bucketed pr22061-1 with the class-(b) leftovers; the
class-(a) table row (C99, "must fix") was the correct classification —
clang 18 accepts and correctly executes the construct under
`-pedantic-errors`, so the clang scope filter never applied.
(2) Torture sweeps now run on the desktop container (QNAP retired from
suites); first container sweep exposed 990413-2's historical pass as
accidental — its x87 `fpatan`/`fsqrt` asm is skipped by the parser and
the uninitialized checked value happened to be ±0.0 on the devbox —
so it moved to the inline-asm skip manifest (60 → 61 skips). Sweep
counts at this stamp (container): **1614/1/9/0/61**, failset exactly
the 10 names above.

**RE-VERIFY 2026-07-22 (owner request, HEAD @a7ed44be — post v0.37.0
script mode + task #91 R0/R1):** full sweep **1614/1/10/0/60**; the
11-name failset is byte-identical to `torture-failset-current.txt` —
zero torture regressions across the #64→#91 span (v0.37.0 release, array
struct members, subscript element reads, frozen values, stream-starter
rider). All 11 remaining are the classified class-(b) GNU-extension
roadmap items (SIMD >16-byte: simd-1/2, pr23135, pr92904, pr46309;
aligned>16/misalign: 20010904-1/2, misalign; pr23324; float→int
saturation pr122000 = task #65; VLA-param stride pr22061-1 = task #80).
**The promote gate (≥1608 class-(a) clean) is MET at live HEAD: 1614 ≥
1608, zero class-(a) failures outstanding.** Same-day companion gates:
dev fulltest 737/0/0/13, `--exe` 723/0, packed arbiter (bin/madc-release)
737/0/0/13. Promote develop→master is the owner's call; on promote, the
MIR fork's `master` begins tracking madc's `master`
(`.claude/rules/build.md` branch correspondence).

**RE-BASELINE 2026-07-19 (task #64, HEAD @1aa53a4e):** full sweep
**1572/32/18/0/63**; the 50-name failset is byte-identical to
`torture-failset-current.txt` (zero regressions across the #35–#63 span).
Live cluster map of the 50: **39 class-(a)** + 11 class-(b); the class-(c)
33 stay manifest-skipped. Two corrections to the tables below, on live
evidence: (1) the "implicit-decl forward call" cluster (5 tests) is a
SYMPTOM of implicit-int DEFINITIONS failing to parse (`mpn_print (){}`
parses as nothing, so the call binds as an undefined import) — it merges
into the K&R/implicit-int parser work item, which now covers 30 of the 39
(the declaration-list form `f(x) int x; {...}` already parses; only the
bare identifier list, empty-parens, and typed-param-after-use forms
remain); (2) pr17078-1 (labels-have-function-scope) is attributed to the
madc CIR builder, not c2mir — stock `c2m -eg` passes it; the `if (0)` arm
fold drops the contained label. Gate math at this baseline:
1572 + 39 class-(a) = 1611 ≥ 1608 — the gate is reachable on class-(a)
alone. Execution-ready tasks filed: #72 (30-test parser lever,
SMAUG-soak-gated), #73 (wide literals), #74 (if-arm label drop). Reducers:
`tmp/s64_knr.c`, `tmp/s64_implicitint.c`, `tmp/s64_wlit.c`.

## Method

Per test: read the source; re-run on CIR capturing the diagnostic
(`tmp/cir_failset_diagnostics.log`); run the same 88 against the asmjit
oracle (`/workspace/madc-asmjit/bin/madc`, log
`tmp/asmjit_oracle_on_failset.log`); cross-check
`docs/parity/root-cause-worklist.md`; tag one of:

- **(a) standard C89–C23** — valid standard C; a real compliance bug madc
  must fix. (K&R old-style definitions count: standard C89–C17, and the
  north-star SMAUG-class codebase is written in them.)
- **(b) GNU extension used by real-world code** — worth supporting
  (computed-goto / case-ranges / zero-length-array precedent).
- **(c) gcc-internal / torture-only construct, or UB-probing** — formally
  skip.

Several attributions were verified with live reducers at HEAD (left in
gitignored `tmp/`), including three where prior worklist attributions were
**stale or wrong** (see "Corrections" below).

## Oracle headline

**asmjit/master passes 82 of the 88.** Only 6 fail on both backends:
`pr22061-3`, `pr22061-4` (nested-fn VLA params), `931018-1` (different bug
per backend), `frame-address`, `pr47237` (`__builtin_apply`),
`va-arg-pack-1`. Five of those six are class (c). Conversely, CIR passes 34
tests asmjit fails (asmjit's total failset is 40) — the backends' capability
sets have diverged; raw "match 1645" is no longer a meaningful definition of
parity.

## Tallies

| class | tests | meaning |
|---|---:|---|
| (a) standard C | **41** | must fix — compliance bugs |
| (b) real-world GNU ext | **14** | support (some roadmapped/deferred) |
| (c) gcc-internal / torture-only / UB | **33** | formally skip |

## Class (a) — standard C, must fix (41)

| cluster | tests | std | notes |
|---|---|---|---|
| **K&R old-style definitions** | 920603-1, 920721-3, 920731-1, 920909-1, 930429-2, 930513-2, 930622-1, 930630-1, 930719-1, 931009-1, 931228-1, 941202-1, 950512-1, 960218-1, 961112-1, inst-check, int-compare, loop-2, loop-2d, loop-3, loop-3b, loop-3c, mod-1 (23) | C89–C17 | "Failed to find type 'x' when parsing function parameters". Untyped identifier lists (implicit int) + the declaration-list form `f(x) int x; {…}`. ONE parser work item; the single biggest lever (23 tests). Distinct from the 2026-06-01 K&R *emission* fix (`ea61546`) — these never get past the parser. Re-run the SMAUG soak after any change here (worklist caveat). |
| **implicit-decl forward call** | 921202-1, 930603-1, 931017-1, 931018-1, cmpsi-1 (5) | C89 | Call to an implicitly-declared function *defined later in the same TU* resolves as an unresolvable external MIR import instead of binding to the in-module definition ("import of undefined item f"). 3-line reducer confirmed at HEAD. SMAUG-class C89 staple. (931018-1 also trips asmjit on a separate extern-const-then-defined pattern.) |
| **implicit-int return on prototype-style defs** | 20031211-2, 950426-1 (2) | C89 | `foo(unsigned int z){…}` — typed params, omitted return type; parser mis-reads the definition as a call expression. Sibling of the K&R cluster, different parse path. |
| **madc keyword leaks into C mode** | pr80153 (1) | C89 | `static const char *string = …` — madc's `string` builtin type is live in C-mode torture runs ("typedef name string as an operand"). A `--std=`/LanguageStd gating bug; any C89 codebase may use `string` as an identifier. |
| **pointer-to-array typing** | strlen-4 (1) | C89 | `*(&a[0][1] - 1)` over `char[2][3][28]` typed as integer → SIGSEGV. Core multi-dim array semantics. (Its declarator half was fixed 2026-06-02; this is the remaining arithmetic/typing half.) |
| **fn-ptr declarator with struct return** | struct-ret-1 (1) | C89 | `X (*fp)(B,char,double,B) = &f;` + `(*fp)(…)` rejected. Matches the known fn-ptr star-recording parser gap (WIP stash exists). |
| **labels have function scope** | pr17078-1, vla-dealloc-1 (2) | C89 label rule (vla-dealloc-1's VLA part is C99) | `goto` to a label inside a dead/inner block → "undefined label"; the branch (and its label) is elided before label resolution. |
| **`_Bool`/enum bitfields + anonymous union** | 20030714-1 (1) | C99/C11 | "_Bool with sign qualifier" ×8 — a signedness qualifier is injected onto `_Bool` bitfields. Ubiquitous flags-word pattern. |
| **`va_list` delegation** | 20041214-1 (1) | C89 | Passing `va_list` by value to a helper that consumes it (every `vfprintf`-style wrapper) SIGSEGVs. Needs a reducer to split from the computed-goto also present. |
| **VLA parameter bound** | pr22061-1 (1) | C99 | `char a[2][N]` with file-scope `N` — wrong row stride at runtime. |
| **wide literals** | 20010325-1, widechar-3 (2) | C89 | `L"…"` is C89. madc's own lowering emits an undefined `__wliteral__*` symbol — finish the Tier-1 lowering (wchar_t array data). Standard, but zero SMAUG pressure → lowest priority within (a). |
| **C23 unnamed-param variadics** | pr117432 (1) | C23 | `void f(...)` + one-arg `va_start(ap)` — standard C23, directly on the north star. Gate via the `--std=` floor. |

Also extracted from a (c) test: **struct-tag shadowing** — `struct s s;`
then `(T)(s.b-8)` resolves `s` as a type, not the variable (live-reduced
from bitfld-5). Tag vs ordinary namespaces are core C89 (`struct stat
stat;`). Standalone (a) work item with no test of its own in the failset;
bitfld-5 itself stays (c).

## Class (b) — real-world GNU extensions, support (14)

| cluster | tests | notes |
|---|---|---|
| **`__int128`** | pr98474, pr122943, pr63302 (3) | **RESOLVED (2026-07-05):** all three now PASS. The 2026-06-11 note ("madc predefines `__SIZEOF_INT128__` without providing the type") is stale — the `__int128`/`unsigned __int128` keyword landed in the P0 value-pool work (pr122943, pr63302), and this date the predefined typedef spellings `__int128_t`/`__uint128_t` landed too (pr98474 used `typedef __uint128_t T;`). Lexer recognizes both `_t` spellings as the canonical 128-bit datatype (atomic, no `signed`/`unsigned` combining), verified byte-identical to gcc. pr98474 removed from `torture-failset-current.txt` (51→50). |
| **SIMD `vector_size`** | simd-1, simd-2, pr23135 (3) | **Stale floor attribution corrected:** the fork @`2ffebff` compiles ≤16-byte vectors; simd-1/-2 now fail only on madc dropping **global vector initializers** to all-zero (reducer-verified). Now a fixable madc front-end bug, Track 1.6 adjacent. |
| **aligned > 16** | 20010904-1, 20010904-2, pr92904 (3) | c2mir caps alignment at 16. GNU `aligned(32)` is everywhere in AVX/cache-line code, and the same floor blocks standard C11 `_Alignas(32)` (extended alignments are implementation-defined, so rejecting is *conforming* — but real code needs it). Fork-level; defer, don't skip. |
| **packed + misaligned access** | misalign (1) | GNU `packed` is ubiquitous (protocol structs). misalign = layout/init bug on packed stores. (20020227-1 — `_Complex float` member in a packed struct — FIXED 2026-07-19 by the task-#69 fork complex-compare conversion fix.) |
| **legacy `__sync_*` atomics** | pr122000 (1) | Pre-C11 atomics used by vast real code; lowerable to C11 atomics/libatomic. Currently an unresolved import. |
| **by-value struct ABI (empty union / zero-len array)** | pr23324 (1) | Trigger is GNU (empty `union {}`), but the SysV by-value classification bug underneath could bite standard structs. Keep in the fix column. |
| **one-void-arm conditional** | pr46309 (1) | `c ? abort() : 0` — GNU (gcc -pedantic rejects; verified), pervasive in assert-style macros; trivial Tier-1 lowering. Second blocker is its empty asm barrier (see skip-list note). |

## Class (c) — formally skip (30)

| cluster | tests | rationale |
|---|---|---|
| **inline asm** | pr40657, pr46309*, pr65053-1, pr65053-2, pr88904, stkalign, misalign* (5 owned: pr40657, pr65053-1/-2, pr88904, stkalign) | c2mir/MIR have no inline asm (documented floor; c11-transpiler rule). All failset uses are empty-string optimizer barriers / stack probes. Optional cheap enhancement (NOT compliance): parse-and-discard clobber-only `asm("":::"memory")` would unlock pr65053-1/-2, pr88904 + pr46309's second blocker. (*pr46309/misalign tabulated in (b); their asm barriers are no-ops to discard.) |
| **gcc stack/apply builtins** | built-in-setjmp, frame-address, pr64242, pr60003, pr47237, va-arg-pack-1 (6) | `__builtin_setjmp/longjmp` (the 5-word-buffer builtin contract, NOT libc setjmp — madc's real `<setjmp.h>`/SJLJ works), `__builtin_frame_address` probing, `__builtin_apply*`, `__builtin_va_arg_pack`. gcc-internal contracts; asmjit itself fails frame-address, pr47237, va-arg-pack-1. Do not alias the builtins to libc setjmp (buffer contract differs). |
| **reduced-precision bitfield arithmetic** | bitfld-3, bitfld-5, pr32244-1, pr34971 (4) | gcc computes oversized-bitfield (`:40`) arithmetic in declared width; **clang aborts on all of bitfld-3/pr32244-1/pr34971 too (verified live)** — under the two-canon rule, matching clang is equally valid. Implementation-defined territory. (bitfld-5's embedded tag-shadow parse bug extracted to (a) above.) |
| **VLA-in-struct / variably-modified members** | 20040423-1, align-nest, 20020412-1, 20041218-2, 20070919-1, pr82210, pr41935 (7) | ISO C forbids variably-modified struct members (C99 6.7.2.1); GNU ext with ~zero real-world use (the kernel purged VLAs), and these tests stack it with by-value `va_arg`, packed `sizeof`, stmt-expr aggregates, variable-index `__builtin_offsetof`. (pr41935's first diagnostic exposes a function-local VLA-typedef parse gap worth a separate reducer someday — the test stays skipped.) |
| **nested fn + VLA param** | pr22061-3, pr22061-4 (2) | GNU nested functions whose params are VLAs with side-effecting bounds mutating the enclosing scope. asmjit also fails both. |
| **`scalar_storage_order`** | 20230630-2, 20230630-4 (2) | gcc-only (clang rejects), bitfield-reversal variant. |
| **UB probes** | 20031003-1 (1) | Out-of-range float→int is UB (C99 6.3.1.4); test pins gcc's compile-time fold against x86 cvttss2si. madc makes a different UB choice. |
| **builtin-recognition** | 20021127-1 (1) | Redefines `llabs` to `abort()`; passes only if the compiler ignores the user's definition. Redefining reserved identifiers is UB; madc's honest call is defensible. |
| **cast-to-union** | 960416-1 (1) | GNU cast-to-union; only such test; Tier-1 lowerable to a compound literal if a real codebase ever needs it. |
| **profiling hooks** | eeprof-1 (1) | Exists to validate `-finstrument-functions` + `__cyg_profile_*`. |

## Corrections to prior attributions (verified this audit)

1. **simd-1/simd-2 are no longer floor gaps** — they compile against fork
   `2ffebff`; the live bug is dropped global vector initializers (front-end).
2. **pr80153 is not an optimizer test for madc** — it's the madc `string`
   keyword leaking into C mode (LanguageStd gating).
3. **The big compile-fail bucket is K&R definition *parsing*** — the 2026-06-01
   "K&R DONE" entry covered emission only.
4. **bitfld-5's compile failure is a tag-shadow parse bug**, not bitfields;
   the test is still unpassable without gcc's 40-bit arithmetic.

## Proposed gate arithmetic

- Suite: 1685. Formal skips (class c): **33** → **in-scope denominator 1652**.
- Current: 1567 in-scope passes (no class-(c) test passes today, so the
  numerator is unchanged).
- **Proposed promote-gate definition:** *all class-(a) tests pass* (1567+41 =
  **1608 minimum**), with class-(b) tracked as roadmap items (the `__int128`,
  SIMD-init, packed, `__sync`, ABI, cond-void clusters: +14 → **1622
  achievable near-term**; aligned>16 is fork-level and may lag).
- For reference: asmjit's 1645 ≈ 1652 minus its own 6 in-scope fails plus
  the 34 (now-CIR-passing) tests it fails. "Match 1645" compares two
  different capability sets and should be retired as the gate wording.

## Work-item view of class (a) — 41 tests, ~12 root causes

**Std-gating directive (user, 2026-06-11):** the K&R / implicit-int /
implicit-decl family is accepted ONLY for STD_MADC and `--std=c89`–`c17`
(mirroring gcc: warn in c99–c17 if desired); it is a **hard error** in
`--std=c23`+ and **all** `--std=c++` modes. The torture runner passes no
`--std`, so the parser fix must work in STD_MADC (which is also how SMAUG
compiles via `--project`).

| # | work item | tests | layer |
|---|---|---:|---|
| 1 | K&R old-style definition parsing (identifier list + decl-list form + implicit int) — std-gated < C23, never C++ | 23 | parser |
| 2 | implicit-decl forward call binds to in-TU definition — std-gated < C23, never C++ | 5 | cir_builder/MIR import resolution |
| 3 | implicit-int return on prototype-style defs — std-gated < C23, never C++ | 2 | parser |
| 4 | labels: function scope incl. dead/inner blocks | 2 | cir_builder label resolution |
| 5 | wide-literal lowering (`__wliteral__*` → wchar_t array data) | 2 | lexer/cir_builder |
| 6 | `string` is NOT a builtin type (user ruling 2026-06-11): retire it to an ordinary header-defined class name with normal lookup/shadowing — the retire-std-hardcoding campaign keystone, NOT a std-gating patch | 1 | parser/DataDef (campaign) |
| 7 | pointer-to-array arithmetic typing | 1 | parser/cir_builder |
| 8 | fn-ptr declarator with struct return (star-recording) | 1 | parser |
| 9 | `_Bool`/enum bitfield signedness | 1 | parser/cir_builder |
| 10 | `va_list` by-value delegation | 1 | cir_builder/fork ABI (reduce first) |
| 11 | VLA parameter row stride | 1 | cir_builder VLA |
| 12 | C23 `f(...)` + 1-arg `va_start` | 1 | parser (std-gated) |
| — | struct-tag shadowing in cast operands (from bitfld-5) | +0 | parser |

## Sign-off record (user, 2026-06-11)

1. **(b)/(c) split: confirmed.** VLA-in-struct stays (c) — clang rejects it
   permanently ("will never be supported", verified live on
   `tmp/vla_in_struct.c`); supporting it would need either variably-modified
   types in c2mir's static layout engine (major fork surgery + by-value
   runtime-sized ABI) or a sweeping madc base+runtime-offset lowering across
   member access/sizeof/assignment/va_arg. Weeks-scale for 7 torture-only
   tests of a gcc-only feature.
2. **Gate wording: agreed** — all class-(a) fixed (≥1608), denominator 1652,
   class-(b) as roadmap items not gate blockers.
3. **Skip manifest: agreed** — the 33 class-(c) tests become formal runner
   skips (data-driven manifest, no per-test branches).
4. **`string` must NOT be a madc builtin type** — pr80153's real fix is the
   retire-std-hardcoding campaign keystone: `string` becomes an ordinary
   header-defined class name subject to normal scope rules (an identifier
   can shadow it, like g++). Neither runner re-moding nor builtin-shadowing
   hacks; the builtin itself goes away.
5. **K&R std-gating** (also recorded at the work-item table): the
   K&R/implicit-int/implicit-decl family is accepted in STD_MADC and
   `--std=c89`–`c17`, hard error in `--std=c23`+ and all `--std=c++` modes.

After sign-off, the gate edits land in `docs/adr/0001-cir-c2mir-backend.md`,
`.claude/rules/branching.md`, `docs/rules/branching.md`, and ROADMAP Track
1.3 (per the handoff deliverable list).

## 2026-07-19 amendment — integer `_Complex` UNSKIPPED (task #69)

The 3-test integer-`_Complex` cluster (20050121-1, complex-6, pr38151) moved
out of class (c): madc now lowers integer-element complex componentwise onto
the struct spine (Tier-1; SysV ABI of `_Complex int` == `struct{T,T}`), with
gcc's tree-complex.cc semantics — including Smith's division in integer
arithmetic ((7-3i)/(-2+5i) = (0,-1), where clang's straight formula gives
(-1,-1); gcc is the arbiter here). c2mir now REJECTS a native integer-complex
specifier instead of silently degrading it to the scalar base (the old
behavior silently dropped imaginary parts — 20041124-1, 20041201-1, pr104604
and pr56837 "passed" only because both sides of their comparisons degraded
identically). Suite lock: tests/testcomplexint.mad. The owner's clang
cross-reference rule (clang-rejected GNU features are out of scope) keeps
this IN scope: clang supports integer `_Complex`.
