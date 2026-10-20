# tinycc → madc: speed and memory recon, phase by phase (2026-10-20)

Recon from reading code; nothing was changed. The phase timings are container measurements (sqlite3.c, `-c`, bin/madc-release); §7 adds the measurements taken after the recon. Any "expected effect" is an estimate and is marked as one. The next step is to replace each estimate with the measurement named beside it (§5).

**Baseline (container, SQLite 3.54 amalgamation, 271,072 lines, `-c`, best of 3, `/usr/bin/time`):** tcc 0.9.27 0.06 s / 18 MB; clang 18 -O0 0.99 s / 182 MB; gcc 13 -O0 2.50 s / 286 MB; **madc-release 2.62 s / 615 MB**; clang -O2 14.9 s / 241 MB; gcc -O2 16.2 s / 365 MB.

Roots: `T:` = /workspace/tinycc @a338258. `M:` = /workspace/madc @24906d71b. MIR sources are under `M:third_party/mir/`.

## 0. What the 1.21 s "other (setup/link)" bucket contains (confirmed by reading)

* `other = total − (read+lex+parse+cir+c2mir+exec+forest)`: M:src/madc.cpp:1903-1936.
* **"c2mir compile" (0.759 s) covers only `cir_compile`.** That is `c2mir_compile_tree`, i.e. `do_context` (the checker), then `gen_mir`, then `MIR_finish_module`: M:src/madc_cir.cpp:1175-1179 and c2mir/c2mir.c:22125-22146.
* **"cir build" covers only `translate_module`:** M:src/madc_cir.cpp:1034-1056.
* **For `-c`, everything after the c2mir timer falls into "other", up to `print_stats()`** (M:src/madc.cpp:2306-2310). That work runs in `madc_cir_emit_native` → `CirJitSession::build` → `load_and_link` (M:src/madc_cir.cpp:3511-3519, 1497-1600). It includes:
  1. **`MIR_load_module`**: one code thunk per function, `_MIR_get_thunk` plus `_MIR_redirect_thunk`, at mir.c:2467-2475. Each one calls `mprotect` twice (mir.c:5249-5258). Data sections are also copied (mir.c:2459-2466).
  2. **`MIR_link`** (mir.c:2551-2676), which does three things:
     * `simplify_func` for every function (mir.c:2575-2583).
     * `process_inlines` for every function (mir.c:2627-2629). It is ON, because madc turns inlining off only under `-g` (M:src/madc_cir.cpp:106-111).
     * The **eager `MIR_set_gen_interface`**, which runs `generate_func_code` for every one of the ~2,672 functions (mir.c:2664-2675; M:src/madc_cir.cpp:1582-1584).
  3. Registering the init array and the alias symbols (M:src/madc_cir.cpp:1732-1740).
  4. `c2mir_get_native_object` → `MIR_gen_object_emit`: the data walk plus ELF assembly (c2mir.c:18476-18480; mir-gen.c:10889-10894). Then one `fwrite` (M:src/madc_cir.cpp:2652-2672).
  5. **Session teardown**, because `session` is a local destroyed before `print_stats`. It runs `cir_finish`, `MIR_gen_finish`, `c2mir_finish` and `MIR_finish`, which frees every item and every insn one at a time (mir.c:1032-1050), then deletes the builder and its CirArena (M:src/madc_cir.cpp:1313-1335).
* **Outside the in-process total but inside wall time:** `~Program` and its member containers, plus static destructors, after `main` returns (M:src/madc.cpp:649, 2309-2310).
* **Hypothesis: confirmed by the code.** Of the 1.21 s, MIR machine-code generation for every function sits inside `MIR_link`. The ELF writer is a small linear tail (§2.6). Teardown is also inside the bucket.
* **Existing split, no code change needed:** `MADC_MIR_CACHE_TIME=1` prints wall-clock laps `load(consumer)` and `MIR_link` (M:src/madc_cir.cpp:1524-1532, 1550, 1587).

## 1. June 2026 tinycc items: did they land?

| June item (plan P#) | status | evidence |
|---|---|---|
| Flat-buffer lexer: slurp the file, inline get/peek, no `istream` (P2) | **Landed** | `Source::_buf`/`_gpos` (M:include/madc.h:2391-2396); inline fast-path `get()`/`peek()` (madc.h:2753-2765, 2831-2836); span scan `fast_span`/`fast_ident_span` (madc.h:2919-2929, 2952-2955; commit be212293f) |
| `CH_EOB` sentinel + `PEEKC` | **Not landed** | The fast path still tests pushback-empty, frames-empty, bounds and three byte values for every char (madc.h:2755-2763). tcc does one compare (T:tccpp.c:729-734, 674-676). Minor. |
| `isidnum_table` | **Landed in equivalent form** | `is_ident_char` is inline arithmetic (M:include/ident_char.h:21-24). A few `isalpha`/`isalnum` remain on cold paths (lexer.cpp:10066-10073). |
| TokenSym interning | **Landed** | `intern_table` (M:include/madcdis/intern_table.h:52-151) uses tcc's exact hash (`hash_step` intern_table.h:113-114 = T:tccpp.c:493-494, commit 256e87d2b). The hash is folded during the identifier scan (M:src/lexer.cpp:9977-10014). A macro probe is a sid lookup (`macro_map.count(sid)` lexer.cpp:10021). |
| TokenSym's **direct binding pointer** (`sym_identifier`, T:tcc.h:438; `sym_find` T:tccgen.c:708-714) | **Not landed** | Each lookup re-interns the istring into the strpool (M:src/parser.cpp:30944), then walks scopes with one `unordered_map` probe per level (parser.cpp:24807-24902). |
| Pre-tokenized macro bodies with replay (P5) | **Not landed** (one partial piece) | `MacroDef::body` is still `std::string` (madc.h:5873). `replacement_tokens` is only a lazily built span cache over the body text, used to match parameters (madc.h:5857-5877; d44776fc0). An expansion is substituted as text, pushed back reversed, and **re-lexed**: function-like at lexer.cpp:10407 via madc.h:2612-2668; object-like "re-tokenize the substituted text" at lexer.cpp:10412-10427. Arguments are collected as `vector<std::string>` plus per-argument `std::set` paint (lexer.cpp:10033-10047). Each expansion frame copies a `std::set` (madc.h:2381, 2436-2439). |
| POD token arena: no per-token `new` (P1) | **Partial** | A bump arena exists (M:include/token_arena.h:46-57; e8861aca0), and a `TokenRec` POD record lives inside every token (M:include/tokens.h:213-230). But each token is still a polymorphic `TokenBase` of about 136 B (vptr plus about 20 fields, tokens.h:231-300). It is never freed (token_arena.h:22-27). The whole TU is lexed into `tokens` before parsing starts (M:src/madc.cpp:1855; lexer.cpp:3842-3844). Pull-based lexing was not done. |
| c2mir registry → bump pages (tcc's TinyAlloc analogue, Aug cold-jit work) | **Landed** | `reg_malloc` uses 256 KiB pages (c2mir.c:415-453; ad9be08d). |

## 2. Phase by phase

Verdict key: **ADOPTED** = madc already does it. **FITS** = it would fit inside the phase; the target location and expected effect are given. **NO** = does not fit, with the reason.

### 2.1 Lex / preprocess (madc: read 0.014 s, lex 0.197 s for 681,521 tokens)

| tcc technique | where (T:) | why it is fast | madc counterpart (M:) | verdict |
|---|---|---|---|---|
| 8 KB `read()` into `BufferedFile`, sentinel at the end | tccpp.c:638-676; tcc.h:629-648 | one compare per char | `_buf` slurp (madc.h:2395, 2548-2549) | ADOPTED. Read is 0.014 s. |
| Hash folded during the identifier scan; chained table | tccpp.c:2671-2694 | bytes are walked once | lexer.cpp:9979-10014 | ADOPTED |
| Tokens are `int`s plus a `CValue`; the parser pulls with `next()`; no token object | tccpp.c:3470-3500 | no allocation, no stored stream | Whole-TU stream of about 136 B heap objects (tokens.h:231-300; lexer.cpp:3844), lexed before parsing (madc.cpp:1855) | NO for dropping token objects: provenance tokens are mandated (MC11-IR). **FITS** for the P1 shrink: hot fields into `TokenRec`, cold ones (`head_tok`, `end_*`, `lex_end_*`, `leading_trivia`, `read_count`) into side tables. Effect: smaller lex write traffic and parse re-read traffic. Ceiling: part of lex 0.197 + parse 0.355. Large job. |
| Macro body stored once as an `int[]`; expansion replays tokens through `macro_ptr` | `define_push` tccpp.c:1252; `begin_macro`/`end_macro` 1053-1075; `next` 3470-3500 | no re-lex; `TOK_GET` copies ints (1221-1230) | Text substitution, then re-lex (§1) | **FITS** (this is P5): store the body as the existing `ReplacementToken` spans, expand into a token run (`TokenStream::splice_front`, the existing owner). Ceiling: the macro share of lex 0.197 s; expected small to medium. Core lexer change, so it needs its own session. |
| One scratch `TokenString` (`tokstr_buf`) and one `CString` reused for arguments | tccpp.c:49; 1006-1048 | no per-invocation heap | New `vector<string>`, `vector<set>`, `std::string` for every invocation (lexer.cpp:10033-10047) | FITS, small: member scratch buffers that are cleared, not reallocated |
| `define_find` = `table_ident[v]->sym_define` | tccpp.c:1274-1280 | O(1) | `macro_map.count(sid)` / `define_map.count(sid)` (lexer.cpp:10021, 10412) | ADOPTED |
| Include guard: skip re-opening (`CachedInclude.ifndef_macro`) | tccpp.c:1392, 1606-1650; tcc.h:686-692 | no re-read | gcc's multiple-include rule (lexer.cpp:7477-7525; madc.h:2508-2520) | ADOPTED |

### 2.2 Parse / declarations / symbols (madc: 0.355 s, decl-parse 0.353)

| tcc | where | why it is fast | madc | verdict |
|---|---|---|---|---|
| The name's TokenSym points straight at its innermost binding (`sym_identifier`, `sym_struct`, `sym_label`) | tcc.h:433-442; tccgen.c:699-730 | lookup is one field read, at any depth | Re-intern the istring (parser.cpp:30944), then probe a hash per scope level (24893-24902); the global scope is `tkProgram`'s index | **FITS** (medium): cache the strpool sid on the istring entry, so the hash is not redone per lookup. A parse-time binding shadow stack keyed by sid would give tcc's O(1). Ceiling: the lookup share of 0.353 s. Unmeasured. |
| Scope exit pops locals; `Sym`s return to a freelist pool | `sym_pop` tccgen.c:783-801; pool 626-665; function end 8628-8630 | O(1) memory per function | Every scope's `variables` plus an `unordered_map` index, kept for the whole TU (parser.cpp:24816-24828) | NO for popping: the AST is mandated. **FITS** for memory: drop a finished function's `var_index` maps. They are self-rebuilding: `var_indexed` reset (24816-24820). Memory only. |
| `static inline` bodies saved as token strings, generated only if used | tccgen.c:8860-8870, 8648-8677, 8679-8688 | dead inline bodies never compiled | Reachability for **system-header** functions (cir_builder.cpp:36320-36360). Every non-system function is a root (36329-36331). | ADOPTED for headers. **FITS** for gcc parity on unreferenced user `static` functions: gcc drops them unless `-fkeep-static-functions` (verify with `nm`). Effect proportional to dead statics in sqlite3.c; unknown, likely small. |

### 2.3 Semantic / types (madc: the c2mir checker inside the 0.759 s; c2mir check vs gen split unmeasured)

| tcc | where | why it is fast | madc | verdict |
|---|---|---|---|---|
| **One pass.** Types are computed while parsing; `CType{int t; Sym*}` is passed by value | tcc.h:457-460 | no second walk; no type allocation for basic types | madc types the AST, then c2mir's `do_context` walks the whole tree again (c2mir.c:12659-12671). Each expression gets `struct expr` plus a fresh `struct type` (`create_expr` 9969-9982, `create_type` 6963-6971), about 190 B kept until `c2mir_finish`. | NO for removing the checker: `gen_mir` consumes its attrs. **FITS** (1): shared immutable basic types instead of a `create_type` per expression; less allocation and memory. **FITS** (2): madc pre-binds each `N_ID` to the decl node it emitted (a cir_node extension field), so `find_def` (7928-7941) skips the per-scope HTAB walk (`symbol_hash` 5950-5958). This is tcc's direct pointer. Effect: the `find_def`/`create_*` share of the checker; unmeasured. |
| Value stack `SValue vstack[512]` holds each expression's value, lvalue bit and constant | tccgen.c:49; tcc.h:479-497, 423 | zero allocation per expression | c2mir `struct expr` per node (above) | NO as a structure. Covered by FITS (1). |
| Constant folding at the operator (`gen_opic`) | tccgen.c:2360ff | folds once | madc folds; c2mir folds again (`const_p`); MIR simplify folds again | NO: the duplication is cheap per node |
| Symbols popped at scope end | tccgen.c:783-801 | small hash/working set | c2mir `symbol_tab` keeps every local of the TU, each with its own `VARR` (malloc) (c2mir.c:5982-5995) | **FITS** (small): allocate the `defs` VARR lazily, only on a second def. Popping locals is NO while check and gen are separate whole-tree passes. |
| `N_ID` strings already interned | — | — | `CirBuilder::id` runs `strlen` twice plus the c2mir string HTAB for every identifier (cir_builder.cpp:248-255) | FITS, tiny: a sid → c2mir string memo |

### 2.4 IR / codegen (madc: c2mir `gen_mir` inside 0.759 s, plus MIR gen inside the 1.21 s)

| tcc | where | why it is fast | madc | verdict |
|---|---|---|---|---|
| **No IR.** `gen_op` → `gen_opi` writes x86 bytes immediately (`g`/`o`/`oad` append straight to `text_section->data`) | x86_64-gen.c:172-190, 248-257, 1712 | 0 passes per insn; ~3-7 output bytes per operation | c2mir builds MIR insns, each malloc'd (mir.c:2769-2775; MIR_op_t 48 B, mir.h:300-318). Then `MIR_finish_func` validates (mir.c:1834ff), `simplify_func`, `process_inlines`, `_MIR_duplicate_func_insns` (a full copy, mir.c:3405-3433), CFG, machinize, loop tree, liveness ×2, RA, combine, DCE, prologue, **two** translate passes, then restore/free of the copy (mir-gen.c:10372-10683). About 15+ walks per insn at -O1. | NO for single-pass codegen: MIR is the backend. The tricks that fit inside MIR are the next rows. |
| No optimization and **no inlining** at all | — | each body generated once | madc default is `-O1` (madc_globals.cpp:26 → MIR level 1, madc_cir.cpp:1362). MIR inlines `MIR_CALL` callees of ≤50 insns and `MIR_INLINE` ≤200, with up to +50% growth (mir.c:4582-4595, 4826-4833). It is turned off only for `-g` (madc_cir.cpp:106-111). gcc -O0 never inlines. | **FITS (rank 1):** at `-O0` do not inline (gcc parity), and benchmark `-O0` against gcc -O0. Level 0 skips the loop tree, the second liveness pass, combine and DCE (mir-gen.c:10536-10539, 10586-10600). Ceiling: most of the 1.21 s; expected the largest single gain. |
| Code goes straight into the final buffer | x86_64-gen.c:172-182 | no copy | gen works on a **duplicate** of the MIR, then restores the original, which is never used again in object mode (mir-gen.c:10405, 10678; mir.c:3435-3464) | **FITS (rank 4):** in object mode, gen in place and free the insns after `target_object_capture` (mir-gen.c:10613-10622). Saves a malloc, a copy and a free per insn and per function; the MIR module's memory goes away progressively. |
| Byte emitters are C code | x86_64-gen.c:355ff, 574ff, 1712ff | direct | Every insn is matched by **parsing pattern text** (`pattern_match_p` mir-gen-x86_64.c:2436ff), sized in pass 1 (3473-3490), and its **replacement string parsed char by char** in pass 2 (`out_insn` 3008-3060, 3505-3512). Patterns are indexed per opcode (2427-2428). | **FITS (rank 5):** decode each pattern and replacement once in `patterns_init` into a compact struct (the operand-constraint vector and the opcode/ModRM/imm recipe); pass 1 reads the cached size. Ceiling: the `target_translate` share of gen. |
| Forward jumps are chains threaded through the code bytes (`gsym_addr`); prologue reserved, then back-patched | x86_64-gen.c:230-238, 1478, 1636-1640 | no label table; no sizing pass | Pass 1 estimates sizes to pick short jumps (mir-gen-x86_64.c:3473-3490) | Folds into rank 5 (the size comes from the cached table). Otherwise NO. |
| Registers are ints; there are no named temps | tcc.h:479-484 | — | **Every temp gets a name:** c2mir `sprintf("I_%u")` + `reg_var_tab` HTAB + `reg_malloc` copy (c2mir.c:12923-12946, 12976-12995). MIR `new_temp_reg` does `sprintf` + `find_rd_by_name` + `MIR_new_func_reg` (name2rdn and reg2rdn HTABs) (mir.c:3046-3062, 1750-1782). c2mir even recovers the type by parsing the name (12955-12974). | **FITS (rank 7):** anonymous temps (desc type, name made lazily for printing). Ceiling: the Ir of the temp path in c2mir and MIR. |
| Values transient on vstack; no CFG and no per-insn data | tccgen.c:49 | — | Per insn `bb_insn`, per bb `bb` plus bitmaps, each malloc'd (`gen_malloc` mir-gen.c:820-836) and freed one by one (`destroy_func_cfg` 2431-2455) | **FITS (rank 6):** a per-function region for gen data, released in bulk by `destroy_func_cfg`. Precedent: c2mir's page arena ad9be08d. |

### 2.5 Register allocation

| tcc | where | madc | verdict |
|---|---|---|---|
| On-demand: `get_reg` scans the vstack, spills the bottom-most user to a temp slot (`save_reg_upstack`, `get_temp_local_var`). Locals live in the frame. No liveness. | tccgen.c:1487-1522, 1406-1440 | MIR builds live ranges (`build_live_ranges` mir-gen.c:7173ff), computes liveness (6757ff) and runs a simplified RA below level 2 (9172-9212) | NO: it needs codegen driven by the parser. The fitting lever is level 0 (rank 1), whose live ranges are already linear (mir-gen.c:7183-7192). |

### 2.6 Object writing / relocations

| tcc | where | why it is fast | madc | verdict |
|---|---|---|---|---|
| Code and data written straight into `Section` buffers; doubling `realloc` | tccelf.c:301-338 | no staging | Code is appended to `obj->text` per function (mir-debug.c:1227-1232). Data comes from the item structs at emit time (mir-gen.c:10731-10850). | ADOPTED |
| Relocations appended as final `ElfW_Rela` at emit time | tccelf.c:784-812 | no conversion | flat `objreloc_t` array (mir-debug.c:1322-1332), mapped once in `MIR_object_emit` (1490ff, 1504-1510) | ADOPTED. Linear. |
| Symbols: ELF symtab with hash | tccelf.c:442-487 | — | item→symbol HTAB (mir-gen.c:1293-1307); `strdup` per symbol (mir-debug.c:1269-1290); linear `find_symbol`/`add_init`, used only a handful of times (1334-1346, 1395-1413) | ADOPTED. Note: the lref lookup is a linear scan per lref data item (mir-gen.c:10793-10796), O(n²) only with computed goto. |
| Write = lay out offsets, then `fwrite` | tccelf.c:3171-3192 | — | single `fwrite` (M:src/madc_cir.cpp:2652-2672) | ADOPTED |
| No JIT artefacts in `-c` | — | — | `MIR_load_module` makes and redirects an **executable thunk per function**, which is 4× `mprotect` (mir.c:2469-2475, 5249-5258, 5279-5287), unused in object mode | **FITS (rank 9):** skip thunks when `object_mode`. A few ms to ~20 ms (estimate). |

### 2.7 Memory management (tcc 18 MB vs madc 615 MB)

**What tcc frees, or never builds:**
* No AST and no token stream.
* Function locals popped at function end (tccgen.c:8628-8630).
* `Sym` freelist pools (tccgen.c:626-665).
* TokenSym/TokenString in TinyAlloc (tccpp.c:138-300).
* Macro token strings freed by `end_macro` (tccpp.c:1063-1075).
* Unused inline token strings freed (tccgen.c:8679-8688).
* Only the output sections grow (tccelf.c:301-316).

**What madc keeps past its last use** (none of it is freed per function):

| structure | last real use in `-c` | actually freed | ref |
|---|---|---|---|
| Lexed tokens, about 136 B × 681k ≈ 93 MB minimum (sizeof estimate), plus parser-made nodes | cir build | never: process lifetime | token_arena.h:22-27; tokens.h:231-300 |
| Program AST, scope indexes, maps | cir build | at `main` return (outside the in-process total) | madc.cpp:649 |
| CirArena: cir_node is about 96 B (`node` 48 B + extension), plus c2mir's per-uid position array | `gen_mir` | session teardown, **after** gen and write | cir_node.h:89-170, 186-240; madc_cir.cpp:1179-1185, 1334 |
| c2mir attrs (`expr`+`type` ≈ 190 B per expression), `symbol_tab` with a VARR per symbol | `gen_mir` | `c2mir_finish`, at teardown | c2mir.c:9969-9982, 5982-5995, 445-453 |
| MIR module insns (48 B per operand) | that function's `MIR_gen` (object mode) | `MIR_finish`, at teardown, one insn at a time; the gen copy is freed per function | mir.c:3435-3464, 1032-1050 |
| gen per-function CFG and bitmaps | the same function | per function (OK) | mir-gen.c:2431-2455 |

**Answer to the brief's question: no.** Neither a function's cir_node nor its MIR is freed after its machine code is emitted. All of it lives until `CirJitSession::teardown` (madc_cir.cpp:1318-1354), and tokens and the AST until process exit.

* **M1 FITS (part of rank 3):** in object mode, right after `cir_compile` succeeds and before `load_and_link`, do two things:
  * Free the c2mir context: `cir_finish`, then reinit or keep only what `-g` needs. Skip this under `-g` (`c2mir_object_attach_debug`).
  * `delete builder`. Copy `tu_init_name()` first (madc_cir.cpp:1382-1394).

  The MIR generator then reuses those pages instead of faulting fresh ones.
* **M2 FITS:** rank 4's per-function free.
* **M3 FITS (rank 2):** skip teardown entirely on the CLI AOT exit path. Flush the output, then `_exit`, as cc1 leaves its GC heap to the OS. Applies to `MIR_finish` / `c2mir_finish` / `~CirArena` / `~Program`; the embedding API keeps teardown.
* **M4 FITS (rank 3, zero-code experiment):** glibc malloc is untuned; there is no `mallopt` and no `madvise` anywhere in src, include or MIR (grep: none). MIR's per-function malloc/free cycles can trim and re-fault the heap top. Try `GLIBC_TUNABLES=glibc.malloc.trim_threshold=…:glibc.malloc.top_pad=…:glibc.malloc.mmap_threshold=…` and separately `glibc.malloc.hugetlb=1` (THP) on the same run. If either wins, make it a `mallopt` at CLI start.

### 2.8 I/O

| tcc | madc | verdict |
|---|---|---|
| `read()` 8 KB chunks (tccpp.c:651) | slurp; read 0.014 s | ADOPTED |
| Headers from disk, guarded (tccpp.c:1392) | Headers from libmadc / pack (owner law); guard skip (lexer.cpp:7477-7525) | ADOPTED / NO (owner law) |
| One output write (tccelf.c:2784ff) | One `fwrite` (madc_cir.cpp:2662) | ADOPTED |

## 3. The "second semantic pass" and MIR passes: what tcc does instead, in short

* **(a) Checker.** tcc never re-checks: types and lvalues ride on `SValue` as the parser reduces (tcc.h:479-497; tccgen.c:3042, 3255). madc's fixed pipeline requires c2mir's checker, because `gen_mir` reads its attrs. What fits is to make it cheaper (§2.3 FITS 1-2, rank 8). A further medium step is a per-top-level-item check→gen interleave in c2mir: C is one-pass, and tcc proves it. That would let a function's attr pages be released after its `gen_mir` (needs a mark/release on `reg_malloc`, removed in ad9be08d because nothing called it). It is a memory lever more than a time lever.
* **(b) MIR passes.** tcc has none. Inside MIR, the levers are ranks 1, 4, 5, 6 and 7.
* **(c) Writer.** It is already tcc-shaped (§2.6). The only extra cost is the JIT thunks (rank 9).

## 4. FITS ranked by expected wall gain on sqlite3 `-c` (estimates; each comes with its confirming measurement)

| # | item | place | ceiling (measured phase) | estimate | confirm with |
|---|---|---|---|---|---|
| 1 | **No MIR inlining at `-O0`** (gcc -O0 parity), and run the benchmark at `-O0` (MIR level 0) | init_contexts / emit paths: madc_cir.cpp:1358-1366, 8512-8519 (add `MIR_set_inline_permission(ctx,0)` when level 0); mir.c:2629 | 1.21 s bucket | largest single item, plausibly a few hundred ms | `MADC_MIR_CACHE_TIME=1 --show-stats -c` at `-O0` vs default. Env-gate the inline print `#if 0` at mir.c:3952-3957 to see the growth ratio. One run with inlining forced off. |
| 2 | **Skip teardown on CLI AOT exit** (M3) | after `emit_native_object` / `cir_write_native_object` (madc_cir.cpp:3518-3519, 8590) → main | inside 1.21 s plus post-main | tens of ms to ~0.2 s | a timer around the `session` destructor (madc_cir.cpp:3519); wall (`/usr/bin/time`) minus the `[stats] total` line on the same run |
| 3 | **Fault and malloc pressure:** glibc tunables (M4); free cir and c2mir before MIR_link (M1) | CLI start; madc_cir.cpp:1584 area | bounded by **system time** | the run's sys share | `/usr/bin/time -v` (minor faults, sys time); `strace -c -f -e brk,mmap,munmap,madvise,mprotect`; rerun with `GLIBC_TUNABLES` |
| 4 | **Object mode: no insn duplication; free each function's MIR after capture** | mir-gen.c:10405, 10613-10622, 10678; mir.c:3405-3464 | inside MIR_link lap | small to medium | callgrind inclusive Ir of `_MIR_duplicate_func_insns` + `_MIR_restore_func_insns` |
| 5 | **Pre-decoded x86-64 patterns** | `patterns_init` mir-gen-x86_64.c:2427; `out_insn` 3008ff; `target_translate` 3466-3515 | inside MIR_link lap | medium | callgrind inclusive Ir of `target_translate`, `pattern_match_p`, `out_insn` |
| 6 | **Per-function gen arena** | `gen_malloc` / `destroy_func_cfg` mir-gen.c:820-836, 2431-2455 | inside MIR_link lap | small to medium | callgrind Ir of malloc/free/realloc under `generate_func_code` |
| 7 | **Anonymous temps (c2mir + MIR)** | c2mir.c:12923-12995; mir.c:3046-3062, 1750-1782 | c2mir 0.759 s + MIR lap | small to medium | callgrind inclusive Ir of `get_new_temp`, `new_temp_reg`, `create_func_reg`, `__vsprintf_internal` |
| 8 | **c2mir checker: pre-bound `N_ID` and shared basic types** | c2mir.c:7928-7941, 9969-9982, 6963-6971; the cir builder's id emission | the checker share of 0.759 s | small to medium | two timers in `c2mir_compile_tree` around `do_context` and `gen_mir` (c2mir.c:22134, 22143); callgrind Ir of `find_def`/`symbol_find`/`create_expr` |
| 9 | **No JIT thunks in object mode** | mir.c:2469-2475 | inside `load(consumer)` lap | ≤ ~20 ms | `strace -c -e mprotect` count (expect about 4 × functions) |
| 10 | P5 pre-tokenized macros; scratch reuse in argument collection | lexer.cpp:10019-10427; madc.h:2612-2690 | lex 0.197 s | small to medium; core parser change, its own session | callgrind Ir under `pushback_macro*`, `get_slow`, argument collection |
| 11 | P1 token shrink, or pull lexing (cache locality) | tokens.h:231-300; madc.cpp:1855 | lex + parse 0.55 s | medium; large job | `perf stat -e cache-misses,dTLB-load-misses`; callgrind `--cache-sim=yes` LL misses in parse |
| 12 | sid cached on istring; binding shadow stack | parser.cpp:30944, 24893-24902 | parse 0.355 s | small | callgrind Ir of `intern_table::intern` under `findVariable` |
| 13 | Drop unreferenced user statics (gcc parity) | cir_builder.cpp:36329-36360 | MIR lap + c2mir gen | depends on sqlite3.c | `nm` local `t` symbol count, madc vs `gcc -O0 -c` |

**Single best first step.** One callgrind run of the unstripped -O2 binary on sqlite3 `-c`, the cold-jit method at docs/plans/2026-08-22-cold-jit-startup.md:215-221. Add `MADC_MIR_CACHE_TIME=1` and `/usr/bin/time -v`. That one run confirms or refutes ranks 2-9 at once. Rank 1 needs only two `--show-stats` runs (`-O0` vs default).

## 5. Cheap instrumentation to add before implementing anything

* `[stats]` lines splitting "other":
  * `MIR_load_module`
  * `MIR_link` (simplify, inline and gen; ideally three timers inside mir.c:2575-2676)
  * object emit
  * write
  * teardown
* `[stats] rss after` each phase, using the existing `madc::detail::process_resident_bytes()` (M:src/madc_posix_io.h:237).
* Counters: CirArena nodes (`pages.size()*4096`, cir_node.h:215-228); c2mir `reg_pages` bytes; MIR insn count before and after inlining (mir.c:4795-4796); TokenArena chunks (token_arena.h:63).
* c2mir: separate `do_context` and `gen_mir` time (c2mir.c:22134-22143).

## 6. Does not fit (one line each)

* Single-pass parse+codegen and dropping the AST or cir_node: both are mandated IR.
* tcc's vstack register allocator: it needs codegen driven by the parser; MIR level 0 is the analogue.
* Throwaway int token stream: tokens are the provenance carrier (MC11-IR).
* Reading headers from disk with caching: owner law says libmadc serves the headers.

## 7. Measured after the recon (container, 2026-10-20, bin/madc-release @94b627053)

`/usr/bin/time` and `MADC_MIR_CACHE_TIME=1 --show-stats -c` on sqlite3.c, three runs each:

| | wall | peak RSS | minor faults | MIR_link lap | load(consumer) | c2mir compile | "other" |
|---|---|---|---|---|---|---|---|
| default (`-O1`) | 2.71-2.85 s | 615 MB | 159.5k | 0.878 s | 0.031 s | 0.752 s | 1.174 s |
| `-O0` (inlining still on) | 2.42-2.66 s | 614 MB | 159.2k | 0.715 s | 0.018 s | 0.784 s | 1.004 s |

* The "other" bucket is **MIR_link 0.88 s** (simplify + inline + code generation of every function) + module load 0.03 s + about 0.27 s of object emit, write and session teardown (not yet timed separately).
* `-O0` saves 0.16 s of MIR_link (about 6% of the wall) with inlining still on: rank 1's estimate ("a few hundred ms") is high; the inline-off share is still to measure.
* 159k minor faults ≈ the 615 MB peak touched once (4 KB pages): every phase's memory is fresh pages, none reused (§2.7).

## 8. The lexer against tcc, measured on the release build (container, 2026-10-20)

SMAUG 1.8 `act_wiz.c` (12,041 lines), `-DREQUESTS -DSMAUG -c`. A symbol-carrying release
(`rm lib/release/libmadc.so bin/madc-release; PATH=<dir with a no-op strip>:$PATH make -C src release`,
the pack attached as shipped), callgrind:

| | madc-release | tcc 0.9.27 |
|---|---:|---:|
| wall, whole compile | 225 ms | 12.5 ms |
| Ir, whole compile | 1,835M | 103M |
| Ir, lexing only (`Program::tokenize`) | **303M** | (inside 103M) |
| lex Ir per token (~107k tokens) | **≈ 2,800** | — |

tcc's whole compile — its system headers lexed too — is under 1,000 Ir per token; madc's lexer alone
is ~2,800. **Correction (c2m front-end recon, same day):** the command-line `-D` in this run declines
the pack bind, so these 303M include lexing the system headers from text (86.4M); pack-bound (defines
in a wrapper file) the lexer is 233M, ~2,385 Ir per token — the `-D` decline is plan item B1 in
[the front-end speed plan](2026-10-20-front-end-speed-plan.md). The release profile is flat:

| where the lexer's Ir go | share |
|---|---:|
| `Program::_getToken` itself | 14% |
| `std::string` work: per-char `push_back`, `==`, `strlen`, `memcpy`, `erase`, `replace` | ~14% |
| per-char `Source::get` / `peek` / `get_slow` (several checks each; no end-of-buffer sentinel) | ~11% |
| `push_token_with_literal_concat` (a second step over every token) | 7% |
| token objects + malloc/free | ~6% |
| name classification by string checks (`compound_type_specifier_flag`, `lexer_type_token`, `madc_slot_id_for`) | ~5% |
| block comments | 3.4% |
| pack decompression (zstd; replaces lexing the system headers) | ~2% |

Macro expansions are substituted as text and re-lexed through pushback frames (§1).

**Fix shape (tcc's, §2.1):** a pointer scan over a sentinel-terminated buffer; intern while scanning
(done); keyword / type class by interned id, never a string compare; no `std::string` per token; literal
concatenation folded into the scan; macros replayed as token runs with the invocation as provenance (P5).
Token objects with provenance stay (MC11-IR) — about 6%. Target: ~5x fewer lex Ir. Core lexer work:
its own focused session (owner parser rule), after the c2mir alias memo and the build-time MIR pattern
tables. KG: `Gap {name:'lexer_cost_vs_tcc'}`.
