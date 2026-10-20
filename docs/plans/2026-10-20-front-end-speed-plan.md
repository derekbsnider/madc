# Front-end speed: beat c2m, then close on tcc (plan, 2026-10-20)

**Goal (owner, 2026-10-20):** madc compiles faster than c2m — MIR's own C compiler, the same back
end — at the same `-O` level, on real C. Otherwise madc is "a bloated, slow clunker sitting on top of
c2mir". KG: `Decision {name:'beat_c2m_compile_time'}`.

**Evidence** (container, callgrind + interleaved wall, every number with its command):
[tinycc recon](2026-10-20-tinycc-recon.md), [c2m front-end recon](2026-10-20-c2m-frontend-recon.md),
[madc parse + CIR cost recon](2026-10-20-madc-frontend-cost-recon.md).

## 1. Where we stand

SMAUG `act_wiz.c` (12,041 lines), `-c`, release builds. madc numbers are pack-bound (the defines in a
wrapper file; see item B1 for the `-D` case):

| phase | madc-release (-O1) | c2m -O1 | tcc |
|---|---:|---:|---:|
| lex / preprocess | 233M Ir | 390M (184M user code + 205M glibc headers) | |
| parse | **369M** | **50M** | |
| AST → cir_node (CIR build) | **195M** | — (its parser builds `node_t`) | |
| c2mir check + gen | 146M | 161M | |
| MIR link + codegen | 760M | 734M | |
| **whole compile** | **1,753M Ir / 210-220 ms** | **1,376M / 151 ms** | **103M / 12 ms** |

* The back ends match; **the gap is parse + CIR build** (+514M Ir). madc's lexer already matches c2m per
  token (1,665 vs 1,547 Ir on identical preprocessed text) — c2m is not the lexer's model; tcc is.
* About half of parse and CIR is overhead `-O2` cannot remove (51% / 44%): RTTI (`dynamic_cast` + its
  typeinfo `strcmp`), re-interning, `std::string` compares, allocation, TLS. Much of it is C++ and
  dialect machinery that runs on C input and can never fire there.
* sqlite3.c: same shape, plus a quadratic `if (x < y)` scan (C9) and the c2mir alias walk (already fixed:
  the type-alias memo, 2,723 → 2,245 ms).

**Target:** act_wiz.c whole compile < 1,376M Ir (c2m) — i.e. remove ≥ 380M Ir — and wall below c2m's
on act_wiz.c and sqlite3.c, with byte-identical objects.

## 2. Method — every item proves itself before it lands

1. **Experiment first.** Each item names its experiment (a callgrind toggle on the function, a call
   count, a synthetic input). Run it on the symbol-carrying release (`rm lib/release/libmadc.so
   bin/madc-release; PATH=<no-op strip>:$PATH make -C src release`), before and after. An item whose
   experiment does not show its gain is dropped, not shipped.
2. **Behaviour oracle: byte-identical objects.** `madc-release -c` on act_wiz.c and sqlite3.c before and
   after; `cmp` must show no difference (every item here is a cost change, never a codegen change).
3. **Gates per commit:** Tier 1 (the touched area's tests) + Tier 2 (`fast_lanes.sh`) + static gates +
   unit + bind + parse-cost; parse-cost rows that drop >0.5% are re-recorded downward.
4. **A standing gate:** `scripts/perf_vs_c2m.sh` (new, item G1) — madc-release vs in-tree c2m `-O1`
   on act_wiz.c and sqlite3.c, Ir ratio and interleaved wall; it reports the ratio every run and, once
   madc is ahead, fails if madc falls behind c2m again.
5. **Rules that bind every item:** C-only work is gated by the LanguageStd owners
   (`presents_as_cpp()`, `dialect_surface_enabled()`, madc.h:6871/6880 — invariants I2/I8, never a
   name test); RTTI replaced through the existing kind-accessor families (datadef.h:531-569,
   tokens.h:403-470); caches live on per-Program or per-object state (thread-safety law: no new bare
   mutable globals); parser edits in this plan are planned gating/caching, never a change to what a
   construct consumes (owner parser rule) — C9 is the one item that changes a scan, and it gets its own
   reducer set.

## 3. Phase A — parse + CIR overhead on C input (≈270M Ir on act_wiz, ≈2.4G on sqlite3)

Ranked by measured Ir (madc recon §4). Each is one commit unless noted.

| # | item | where | measured cost (aw / sq) | experiment |
|---|---|---|---:|---|
| A1 | `dd_is_platform_integer`: 4 failing `dynamic_cast`s, up to 15 per `usual_arithmetic_result`, testing classes never minted on LP64 Linux → a kind accessor | datadef.h:2120-2126; tokens.h:656-729 | 64.6M / 593.6M | callgrind toggle `usual_arithmetic_result`: 64.6M → ~5M |
| A2 | C++ class-operator machinery on every C operator → return early when `!presents_as_cpp()`; `upcast_class_ptr` tests the class pointer first | parser.cpp:22151/22348/22478; cir_builder.cpp:14198/23267/7069/21352 | 30.3M excl. A1 / ~800M incl. | toggle the entry points; `cmp` objects |
| A3 | identifier arm re-interns each spelling ~9× and strcmp's it ~12× → one `spelling_name()` (the existing memo) passed down; identity compares | parser.cpp:41811ff; tokens.h:1671 | 42.9M / 302.2M | `istring(const char*)` calls from the arm: 166K → ~0 |
| A4 | other hot `dynamic_cast`s → the existing token / DataDef accessors | parser.cpp:18485-18592, 41576; cir_builder.cpp:6571, 14511; … | ~32M / ~250M | `__dynamic_cast` calls 383K → < 60K |
| A5 | CIR referenced-surface filter: `std::set<istring>` re-interning c2mir's unique strings → keyed on the c2mir pointer | cir_builder.cpp:35623-35670 | 30.3M / 238.2M | toggle `cir_harvest_type_refs` |
| A6 | eager `count_queued_call_arguments` for a `--std=madc`-only UFCS check → gated on `dialect_surface_enabled()`, counted lazily | parser.cpp:43668-43673, 32926 | 18.3M / 89.2M | calls 18,737 → 0 on C |
| A7 | literal compare ladders: `integer_promoted_type` resolves "int" through a 60-row string table per call; eval-scope helpers do 10 compares per C call | parser.cpp:63817-63849, 4088-4150, 724, 30781 | ~15M / ~170M | `std::operator==(string,const char*)` 858K → < 100K |
| A8 | each identifier looked up twice (contextual pre-check, then resolution) → reuse the first result; `findParameter` by pointer | parser.cpp:5184, 30936-30945, 24930 | 13.1M / 81.3M | `findVariable` 31,829 → ~13,400 |
| A9 | **quadratic** `if (x < y)` probe: a C++ condition-declaration scan with a bare `DelimDepth d;` runs to the next `>` (≤4,096 steps) → skip in C, gate on a template name, `DelimDepth d(this)` (the delimiter rule) | parser.cpp:55392-55393, 14583-14612 | 8.5M / 213.8M | synthetic `ifscan_lt.c` = `ifscan_le.c` |
| A10 | `DBG()` reads a `thread_local` through `__tls_get_addr` (837K calls) → `tls_model("initial-exec")` or a plain process-wide pre-test | datadef.h:25-27 | 7.5M / 40.8M | `__tls_get_addr` calls → ~0 |
| A11 | a `std::deque` per expression-stack (two per `parseExpression`) → `std::vector`-backed `std::stack` | madc.h (18) + parser.cpp (25) | 7.1M+ / 51M+ | `_M_initialize_map` → 0 |

## 4. Phase B — the pack on the common path

| # | item | cost | experiment |
|---|---|---|---|
| B1 | **A command-line `-D` declines the pack bind** (exact match includes the `-D` hash, lexer.cpp:4849-4850, cir_freeze.cpp:142-155): every system header is lexed from text. Owner law: never decline the pack — make the bind handle it (a define no header tests cannot change them; K2 already re-evaluates external conditionals in the consumer state). KG `Gap pack_bind_declined_by_command_line_D` | act_wiz +81.4M Ir / ~10 ms; every real build passes `-D` | act_wiz with `-D…` equals the wrapper build in tokens lexed (97,851) and Ir |

## 5. Phase C — the lexer, tcc's shape (target ≈ 5× fewer lex Ir)

| # | item | measured | experiment |
|---|---|---|---|
| C1 | keyword / type class memoized per `spelling_id` (c2m `kw_add`, tcc `TokenSym`) | 15.6M (5.2% of lex) | toggle tokenize on `aw_pp.c` (177.4M baseline) |
| C2 | `nextToken` per-token statistics compiled out of release; cursor cost → index | 16.1M (87 Ir/token) | toggle `Program::parse` |
| C3 | end-of-buffer sentinel + inline pointer scan (tcc `PEEKC` / `CH_EOB`); `Source::get` stops checking pushback/frames per char | `get`/`peek`/`get_slow` ~11% of lex | toggle tokenize on `aw_pp.c` |
| C4 | no `std::string` grown per char per token; literal concatenation folded into the scan | std::string ~14%, concat 7% | same |
| C5 | macros replayed as token runs — **tcc's allocation-free replay, not c2m's** (c2m's own replay costs ≥ 49M on act_wiz); provenance: the invocation + the definition token | ceiling ≤ ~50M (wrapper − pure-lex differential) | bytes lexed from pushback frames × 321 Ir/byte |

## 6. Phase D — structural (after A–C, re-measured)

Two interning layers (global `istring_table` + per-Program `strpool`), virtual `id()`/`type()`/`datadef()`
leaf accessors (lexed tokens already carry `rec.kind`), per-node `CirBuilder::make` bookkeeping (22M aw),
`c2mir_next_uid` per-node VARR push (bulk grow). Planned from the post-A–C profile, not before.

## 7. Back end (shared with c2m: speeds both, never closes the gap)

Done: the c2mir type-alias memo (gcc `TYPE_ALIAS_SET`). Queued: MIR x86-64 pattern tables decoded at build
time (all five MIR targets; ~4% of sqlite3); no MIR inlining at `-O0` (gcc parity); skip teardown on the
CLI AOT exit; free cir_node / c2mir data before `MIR_link` in object mode (peak RSS).

## 8. Order

1. G1 `perf_vs_c2m.sh` (records the starting ratio).
2. Phase A, one item per commit, A1 → A11 (A9 with its own reducers: `if (a < b)` / `if (a <= b)` /
   a C++ condition declaration `if (T<int> x = …)`).
3. Phase B (B1).
4. Phase C — core lexer work: its own focused session (owner parser rule), planned here.
5. Re-profile; Phase D from that profile.

**Thread-safety contract** (every item): caches are per-Program or per-object (the alias memo is per
gen_ctx); no new process-wide mutable state except A10's read-only TLS-model change.
