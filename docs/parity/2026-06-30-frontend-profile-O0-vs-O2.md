# Front-end profiling record — -O0 vs -O2 (2026-06-30)

Durable record of the profiling session that re-evaluated rung-1 Step 4
(`TokenIdent::str` drop) against current data. Scratchpad `cg*.out` files are
ephemeral; THIS doc is the kept record. Re-measure after any front-end change.

## Setup
- **HEAD:** `3c4adad9` (develop, rung-1 Step-3 complete; lexer rolling-hash already landed).
- **Host:** madc-devbox, 4 cores. Dev binary is **-O0** (src/Makefile default).
- **Method:** `bin/madc --show-stats <file>` for phase wall-clock (best of 3);
  `valgrind --tool=callgrind` for self-cost attribution. callgrind of a full -O0
  parse (~50x) exceeds the sandbox CPU cgroup → -O0 attribution taken on a small
  reducer (sandbox disabled for that run); -O2 (1.74x faster) fits.
- **Inputs:**
  - `tests/testmap.mad` — 2208 KiB, 170756 tokens produced, 337654 consumed (1.98x).
  - reducer = `#include <map>,<string>,<vector>` + `main` doing 5 `m[i]=`/`v.push_back` (144324 tokens, 4042 instantiate calls).

## Phase timing — `--show-stats` on tests/testmap.mad

| phase            | -O0 (best of 3)        | -O2 (best of 3)        | -O2 speedup |
|------------------|------------------------|------------------------|-------------|
| read             | 0.003 s                | 0.003 s                | —           |
| lex              | **0.204 s** (~838K tok/s) | **0.158 s** (~1.08M tok/s) | 1.29x  |
| parse            | **0.824 s** (~410K tok/s) | **0.272 s** (~1.24M tok/s) | **3.03x** |
|   └ instantiate  | **0.595 s (72% of parse; 4122 calls)** | **0.195 s (72% of parse)** | 3.05x |
|   └ decl-parse   | 0.229 s (PCH-cacheable share) | 0.077 s          | 2.97x       |
| c2mir compile    | 0.010 s                | 0.011 s                | —           |

(Earlier same-session -O0 testmap runs: parse 0.837 / 0.865 / 0.824; instantiate 72% stable.)

**Delta read:** -O2 makes parse **3.0x** faster — most of that is the ~12% trivial
accessor/iterator non-inlining (pure -O0 artifact) PLUS inlining of hashing/string
helpers into their callers. **`instantiate` stays 72% at BOTH levels** → the proportion
is opt-invariant; instantiation is the genuine dominant algorithmic cost, not an -O0 mirage.

## callgrind self-cost — -O0, reducer (3.73 B insns total; madc-side, startup/linker filtered)

| % (self) | function | class |
|---------:|----------|-------|
| 6.52% | `intern_table::hash_step` | hashing — LEGIT (per-char rolling fold) |
| 5.17% | `intern_table::hash_bytes` | hashing — **REDUNDANT** (2nd byte-pass; see callers) |
| 1.75% | `Program::_getToken` | lexer |
| 1.70% | `Source::get` | lexer |
| 1.58% | `Program::finalize_pop1_rec` | per-token rec finalize (interns file_id/spelling) |
| ~12% (sum) | `__normal_iterator` ctors, `vector::empty/end/begin/back/size`, `operator-` | **PURE -O0 OVERHEAD** — -O2 inlines to nothing |
| 1.01% | `arena::alloc` | token arena |
| 0.94% | `std::operator==`(string,char*) | residual std::map / string compares |
| 0.92% | `detect_include_guard` | lexer |
| 0.86% | `intern_table::intern` (3-arg) | interning |
| 0.61% | `despace_spelling` | lexer type-spelling |
| 0.58% | `basic_string::_M_construct<char*>` | std::string ctor (incl. `.str`) |
| 0.49%+0.49% | `std::less<string>` / string `operator<` | residual std::map comparator |

### Where the REDUNDANT hashing comes from (hash_bytes callers, inclusive)
`hash_bytes` is reached **223,512×** exclusively via the non-precomputed
`intern(const char*, len)` / `intern(string)` path. Top inclusive callers:
- `complete_pending_template_instantiations` — 10.78%
- `CirBuilder::translate_module` lambda — 10.45%
- `parseExpr_identifierArm` — 7.33%
- `resolve_typename_type_token` — 6.28%
- `resolve_declared_type_token` — 5.03% + 1.23%

→ The redundant hashing is driven by **template instantiation + type resolution
interning long mangled-name keys**, NOT the lexer and NOT `.str`. It is part of
the 72%-instantiation cost, and the rolling-hash (lexer-only) does not cover it.

## Read (decision-relevant)
- **`.str`-drop (rung-1 Step 4) touches ~1%** (`_M_construct` 0.58% + part of
  `finalize_pop1_rec`) and does NOT touch the instantiation-path hashing. Its
  value is architectural (POD tokens, find(id) lookups, -32B/token), not perf.
- **Two real -O0 algorithmic levers** that survive -O2:
  1. Redundant `hash_bytes` (5.17%) — route the instantiation/type-resolution
     intern callers through precomputed-hash / id-keyed lookups (avoid re-hashing
     mangled-name keys). Tied to the instantiation track.
  2. `intern_table` over-reservation — each ctor `reserve(1<<16, 1<<12)` fills 4096
     buckets; the small dedicated pools (type/template/namespace) pay it needlessly
     (startup-scale; 51x under --project). Cheap right-size.
- **~12% of -O0 is trivial accessor/iterator non-inlining** — pure -O0 artifact,
  -O2 erases it. Do NOT spend effort there.
- **Instantiation is 72% of parse** — the dominant lever (embedded-header-forest /
  instantiation-caching track), not rung-1 Step 4.

## callgrind self-cost — -O2, reducer (1.48 B insns total = 40% of -O0's 3.73 B; madc-side filtered)

| % (self) | function | note |
|---------:|----------|------|
| **7.59%** | `TokenCpnd::findVariableThisScope(intern_table&, uint, string&)` | **NEW #1 — variable scope lookup; INVISIBLE at -O0 under iterator noise** |
| 4.41% | `Program::_getToken` | lexer char dispatch |
| 4.29% | `Source::get` | lexer char read |
| **3.99%** | `Program::finalize_pop1_rec` | per-token rec finalize (interns file_id EVERY token; spelling) |
| 2.33% | `detect_include_guard` | lexer |
| 2.16% | `intern_table::intern` (3-arg, precomputed) | interning (hash now INLINED in here, not separate) |
| 2.10% | `std::operator==`(string,char*) | string compares (scope/keyword) |
| **1.83%** | `TokenCpnd::findVariable` | more variable scope lookup (→ ~9.4% combined with #1) |
| 1.59% | `Source::peek` | lexer |
| 1.43% | `TokenBase::operator new` | token arena alloc |
| 1.02% | `despace_spelling` | type-spelling normalize |
| 0.86% | `std::_Rb_tree<string>::find` | **residual std::set<string>** — NOT yet on intern_keyed_map |
| 0.80% | `std::_Hashtable<uint,Variable*>::_M_emplace` | variable hashtable insert |
| 0.75% | `register_outofline_member_instantiations` | instantiation |
| 0.72% | `basic_string::_M_construct<char*>` | std::string ctor (incl. `.str`) — **~0.7%** |
| 0.59% | `c2mir_node_op` | backend (small) |
| 0.55% | `std::_Rb_tree<string,TokenFunc*>::find` | **residual std::map<string,TokenFunc*>** — re-key candidate |
| 0.46% | `intern_keyed_map<TokenDataType*>::find(string)` | string-keyed find still hashes (id-keyed would skip) |

### What -O2 reveals that -O0 hid
- **Hashing vanished from the top list:** at -O0 `hash_step`+`hash_bytes` = 11.7%; at -O2
  they are INLINED into `intern`/`finalize_pop1_rec`/`findVariable*` callers (the attribution
  shift the Makefile comment warns about). The work didn't disappear — it moved into callers.
- **Variable scope lookup is the real #1** (`findVariableThisScope` 7.59% + `findVariable`
  1.83% ≈ **9.4%**). Hidden at -O0 because its cost was spread across un-inlined iterator/
  string helpers. **This is the highest-value -O2 algorithmic lever** and was NOT on anyone's
  radar. Worth investigating: it takes `(intern_table&, uint id, string&)` — already partly
  id-aware, but the `string&` + the 2.10% `operator==(string,char*)` suggest a linear scope
  scan with string compares rather than a pure id-indexed lookup.
- **`finalize_pop1_rec` 3.99%** persists at -O2 — interning `file_id` for EVERY token (same
  filename re-hashed 144K times) is avoidable waste; cache file_id per source file.
- **Residual std::map/set<string>** (`_Rb_tree<string>` 0.86% + 0.55%) — more keyed maps not
  yet on `intern_keyed_map`; the rung-1 re-key campaign isn't actually complete.
- **`.str` malloc ~0.7%** at -O2 — confirms the Step-4 perf payoff is ~1%, architectural-only.

## Updated priority (honest -O2, shipping-level)
1. ✅ **DONE (commit 5739e624)** — Variable scope lookup re-hash + finalize file_id re-hash.
   `Variable::name_sid` cache (findVariableThisScope absorbs without re-interning names) +
   single-entry file_id cache in finalize_pop1_rec. Deterministic callgrind below.
2. **Instantiation (72% of parse, opt-invariant)** — the dominant structural lever
   (embedded-header-forest / instantiation caching). Largest but biggest project.
3. **Residual std::map/set<string> re-key (~1.4%)** — finish the rung-1 re-key campaign.
4. **`intern_table` over-reservation** — right-size the small dedicated pools. Cheap.
5. **`.str`-drop (rung-1 Step 4, ~1%)** — do for architecture (POD tokens / find(id)), not perf.

## RESULT — Step 4 complete: TokenIdent::str dropped (commit d706c521)
Rung-1 interning Step 4 done in gated tranches (T1 f98772ae, T2 679c3045,
T3 53578349, T-final-A fd540cab, T-final-B d706c521). Bare identifier tokens no
longer carry a per-token `std::string`; spelling lives in the interned pool
(rec.spelling_id via TokenBase::_active_strpool). Content/alias subclasses
(TokenStr/TokenREM/TokenKeyword/TokenDataType) retain their own `str` + override
the virtual spelling().

Deterministic callgrind, small reducer (`#include <map>`), -O0, cumulative since
baseline 6ac620af (includes name_sid/file_id + Step 4):

| metric | baseline 6ac620af | rung-1-complete d706c521 | delta |
|--------|------------------:|-------------------------:|-------|
| total instructions | 1,773,514,596 | 1,714,854,313 | **-3.3%** |
| madc `_M_construct<char*>` (token std::string ctors) | 9.26M (0.52%) | 5.27M (0.31%) | **-43%** |
| `basic_string::_M_construct` Guard ctor+dtor (madc) | 3.54M | 1.96M | -45% |

Bare identifiers (the most numerous token) went from a 32-byte std::string + heap
buffer to a 4-byte interned id. Residual `_M_construct` is content-subclass str +
other std::string usage. Gates: fulltest 673/0/0/16; torture == 51 baseline.

## RESULT — name_sid + file_id cache (commit 5739e624)
Deterministic callgrind, small reducer (`#include <map>` + trivial main), -O0:

| metric | baseline 6ac620af | fixed 5739e624 | delta |
|--------|------------------:|---------------:|-------|
| total instructions | 1,773,514,596 | 1,657,116,820 | **-116.4M (-6.6%)** |
| `hash_step` | 85.6M (4.83%) | 32.6M (1.97%) | **-62%** |
| `hash_bytes` | 64.5M (3.64%) | 21.9M (1.32%) | **-66%** |

Hashing total 150M -> 54.5M insns (~64% cut). Wall-clock parse delta on testmap is
within this host's ~3% jitter (alternating A/B: baseline ~0.927s vs fixed ~0.918s),
so the **instruction count is the load-bearing measurement**, not wall-clock.
Gates: fulltest 673/0/0/16; 0 warnings; tag-arith 0/0; torture failset == 51-name baseline.

NOTE method: build pre-change binary in a `git worktree` at the baseline commit, callgrind
BOTH on the same input; Ir counts are deterministic (no host noise). -O0 full-parse callgrind
exceeds the sandbox CPU cgroup → use a SMALL reducer (the delta is still exact).
