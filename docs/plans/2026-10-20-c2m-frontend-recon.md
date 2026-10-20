# c2m (c2mir) front end vs madc: measured recon (container, 2026-10-20)

Recon only: nothing in any repo was edited, built or committed. Every scratch file and callgrind output is in the container's `/tmp/tsprobe/c2mrecon`. The c2mir.c, c2mir-driver.c, parser.cpp, lexer.cpp, tokens.h, datadef.h and madc.h used on the NAS and on the container are byte-identical (md5), so `c2mir.c:N` is the source of the measured `obj/mir/host/c2m` (-O3 -g). madc is `bin/madc-release` plus the unstripped `lib/release/libmadc.so`.

`$S` = `/workspace/MadSMAUG/upstream/smaug1.8/src`. `AW` = `-DREQUESTS -DSMAUG -I$S $S/act_wiz.c`.

## 0. Headline

1. **madc's lexer is not slower than c2m's preprocessor.**
   * On the same gcc-preprocessed text (552,351 bytes, no macros or directives), pure lexing costs the same in both:

     | | Ir | tokens | Ir/token | Ir/byte |
     |---|---:|---:|---:|---:|
     | madc | 177.4M | 106,549 | 1,665 | 321 |
     | c2m | 175.4M | 113,353 | 1,547 | 318 |

   * On the raw act_wiz.c, madc is cheaper: tokenize 302.7M against c2m's `processing` 389.5M. c2m loses its time on glibc headers (§5).
   * **c2m is not the model for the lexer; tcc is** (<1,000 Ir per token for its whole compile, tinycc recon §8).
2. **The madc−c2m gap is all parse plus CIR build.**
   * act_wiz: front end 879.3M Ir in madc against 439.0M in c2m.
     * parse +336.7M (386.2M vs 49.5M)
     * CIR build +190.4M (c2m has no such pass)
     * lex −86.8M
   * sqlite3: the whole-compile gap is 3,288M Ir. Front end 5,995M in madc against 2,707M in c2m, which is exactly the gap.
     * parse 2,713M vs 319M
     * CIR 1,456M
     * lex 1,826M vs 2,388M
3. **c2m's measured front end (pre + parse) on act_wiz is 50.5-53 ms, not ~27 ms.**
   * glibc headers alone are 22.7 ms.
   * The user-code-only part is about 27 ms, so the inference was right only for user code.
4. **Not a c2m item, but it skews the baseline.** Any command-line `-D` makes madc decline the header pack and lex the system headers live.
   * Cause: an exact pack match requires the `-D` hash (lexer.cpp:4849-4850; hash at cir_freeze.cpp:142-155, nonzero for any `-D`).
   * act_wiz with `-DREQUESTS -DSMAUG` lexes 460,866 header bytes (8,726 tokens, 86.4M Ir).
   * The same program with the defines in a wrapper file binds the pack: 544,491 input bytes, 97,851 tokens.
   * Cost: +81.4M Ir for the whole compile (1,834.5M vs 1,753.1M), about 10 ms wall (220-230 vs 210-220 ms, interleaved).
   * The brief's "240 ms, lex 39 ms" baseline includes this. The tinycc recon §8 line "without lexing the system headers (the pack binds them)" does not hold for `-D` builds.

## 1. Q1: c2m's phase split

ms come from `c2m -v` (its own timestamps, c2mir.c:21931-21983; the driver's "MIR object link+gen" timer wraps `MIR_link` only, c2mir-driver.c:1007-1010), best of 3. Ir are callgrind inclusive. Commands:

```
c2m -v -w -O1 AW -fobject -o aw.o
valgrind --tool=callgrind c2m -w -O1 AW -fobject -o aw.o
callgrind_annotate --inclusive=yes --tree=calling --threshold=100
```

A "≈" in the ms column is Ir ÷ that phase's measured Ir/ms.

### act_wiz.c

Whole run: 1,376.0M Ir; wall 150-160 ms (`/usr/bin/time`, interleaved); peak RSS 61.5 MB (madc: 108 MB).

| phase | functions summed | Ir | ms |
|---|---|---:|---:|
| file read | driver slurp: `getc` per byte, 341,723 calls (c2mir-driver.c:928), 7.5M; includes `fgetc` per char inside `cs_get` (c2mir.c:1089-1092), 24.6M; `t_getc` + `top_level_getc` 4.8M | 36.9M | ≈4 (inside pre) |
| preprocess + lex + macros | `processing` (called from `pre`, which is inlined into `c2mir_compile`) | 389.5M | 43.0-44.6 |
| – of which the lexer | `get_next_pptoken_1`, all callers, 386,762 calls | 309.4M | |
| – of which skipped lines | `skip_nl` (92.5M of it in system headers) | 94.1M | |
| – of which glibc headers | header-only stub `hdrs.c`: same 14 system includes, `processing` | 205.3M | 22.7-23.1 |
| parse | `compound_stmt` 29.1 + `declaration` 18.9 + `declarator` 1.2 + `declaration_specs` 0.13 + `tpname_add`/`new_node*` 0.1 (`parse`/`transl_unit` are inlined into `c2mir_compile`) | 49.5M | 7.5-8.4 |
| check | `do_context` | 63.1M | 12.0-14.2 |
| MIR generation | `gen_mir` (plus `MIR_finish_module`, 5 Ir) | 97.5M | 17.0-17.4 |
| MIR link + codegen | `MIR_link` (simplify, inline, `generate_func_code` ×164) + `MIR_load_module` 0.3M | 734.3M | 72.8-75.0 |
| object writing | `c2mir_get_native_object` | 5.4M | ≈0.6 |
| teardown | `MIR_finish` 13.3 + `MIR_gen_finish` 5.0 + `compile_finish` 4.8 | 23.1M | ≈2.6 |

Counts: 181,076 pp tokens (whitespace runs included), 107,188 parse tokens. The header stub alone has 26,265 pp tokens and 9,323 parse tokens. User code is therefore 154,811 pp tokens and 97,865 parse tokens; madc lexes 97,851 tokens on the same user code.

### sqlite3.c

9,580,800 bytes. Whole run: 12,148.7M Ir; wall 1.59-1.77 s; RSS 411 MB.

| phase | Ir | ms |
|---|---:|---:|
| file read (driver `getc` slurp, 9,580,801 calls) | 211.1M | ≈22 |
| `processing` | 2,387.8M | 268-275 |
| – lexer (`get_next_pptoken_1`, 2,228,500 calls) | 1,991.0M | |
| – `cs_get` (12,401,366 calls, 90 Ir/char) | 1,119.1M | |
| – `skip_nl` | 716.0M | |
| parse (`compound_stmt` 218.1 + `declaration` 70.3 + `declarator` 24.2 + `tpname_add` 2.9 + `declaration_specs` 2.5 + `new_node*` 0.8) | 318.7M | 54-68 |
| check (`do_context`) | 320.2M | 80-96 |
| `gen_mir` | 1,469.1M | 197-200 |
| `MIR_link` | 7,093.4M | 990-1,084 |
| object (`c2mir_get_native_object`) | 17.6M | ≈2 |
| teardown (`MIR_finish` 119.3 + `MIR_gen_finish` 55.5 + `compile_finish` 10.3) | 185.1M | ≈18 |

Counts: 1,025,444 pp tokens, 673,070 parse tokens.

### madc on the same inputs

The same callgrind method, `bin/madc-release -c`:

| | tokenize | parse | CIR build | c2mir check + gen | MIR_link | total |
|---|---:|---:|---:|---:|---:|---:|
| act_wiz, `-D` | 302.7M | 386.2M | 190.4M | 146.1M (47.2 + 98.8) | 759.7M | 1,834.5M |
| act_wiz, wrapper (pack bound) | 233.4M | 369.3M | 195.3M | 146.1M | 759.8M | 1,753.1M |
| sqlite3 | 1,826.2M | 2,713.4M | 1,456.0M | 1,803.9M (312.2 + 1,491.7) | 7,184.6M | 15,437.5M |

The back ends match. c2m's check + gen is 160.6M on act_wiz and 1,789M on sqlite3, against madc's 146.1M and 1,804M. madc's `do_context` on its own tree is cheaper (47M vs 63M).

## 2. Q2: how c2m's front end is built (c2mir.c unless noted)

**Input**
* The driver slurps the top-level file with `getc` per byte into a VARR (c2mir-driver.c:922-932). c2mir then re-reads it through a function pointer per char: `t_getc` (driver 472-475) via `top_level_getc` (21915).
* Each include is `fopen` + `fgetc` per char (`add_include_stream` 2364-2382, `ln_get` 1089-1092).
* Every line goes through:
  * `get_line` 1103-1120: VARR push per char, trigraph pass (1058-1087), reverse (1094-1101);
  * then `cs_get` (1122-1140) pops one char at a time, and `cs_unget` (1142-1148) pushes back.
* There is no slurp and no sentinel.

**Tokens**
* `struct token` (721-728) is 48 B: code:16, processed_p:16, `pos_t {fname, lno, ln_pos}` (91-94), node_code, a pre-built leaf `node`, and an interned `repr`.
* Allocated by `new_token` (897-908) through `reg_malloc`: a bump allocator over 256 KiB pages, 16-byte aligned (416-445). Pages are freed only at `c2mir_finish` (`reg_pages_finish` 447-455).
* Every token carries file/line/col provenance.
* Whitespace and comment runs are tokens too, with their text interned (1391-1449).
* The lexer is a pull-style function over a char stream (1381-1833). Pushed-back tokens are served first from a stack (`buffered_tokens`, 1385-1386, 1858-1860).

**Interning**
* `str_add` (493-508): an HTAB of `tab_str_t` (`mir_hash` over the bytes, 464-466), with a copy into `reg_malloc`.
* An identifier is interned at lex time and gets an `N_ID` node immediately (`new_id_token` 1372-1379, `new_str_node` 878-882).
* Keywords sit in the same table with `key` = token code (`kw_add` 5749-5751). `pptoken2token` re-looks-up every identifier to obtain that code (1955-1962).

**Macros**
* A macro is `{id, params VARR, replacement VARR(token_t), ignore_p}` (2114-2119). The body is tokenized once at `#define` (`define` 2435-2533; `##` is marked `T_RDBLNO` at 2492-2497).
* `macro_tab` is an HTAB keyed by `mir_hash(strlen(repr))` (2205-2211), probed for every identifier in the main loop (3831-3832).
* **Expansion replays token copies; it does not re-lex.**
  * Object-like (3936-3945): push an EOR marker, `new_macro_call`, `add_tokens`, `do_concat`, then `copy_and_push_back`. Copies take **pos = the invocation's position** (2564-2580, `copy_token` 910-915), so the definition-site position is lost.
  * Function-like: `try_param_macro_call` (3723-3747) → `find_args` (2750-2838; one VARR per argument) → `process_replacement` (2939-3018).
* `#` → `token_stringify`, which builds text (1887-1914).
* `##` → `token_concat` **re-lexes the pasted text** through a string stream (2840-2856).
* **Argument pre-expansion**: the argument copy is pushed between BOA/EOA markers (2998-3007). The main loop expands it into `output_buffer` while `macro_call_stack` is non-empty (`out_token` 3078-3087). At EOA, `add_arg_tokens` (2864-2873) moves the result into the call's `repl_buffer` (3819-3826).
* **Painting**: `m->ignore_p` is TRUE while the call is active (2961, 3944), cleared at EOR by `pop_macro_call` (2738-2748). An identifier met while its macro is ignored is painted permanently as `T_NO_MACRO_IDENT` (3931-3934), and turned back into `T_ID` at 1954.
* **Per-call allocation**: a malloc'd `macro_call` (2272-2281), argument VARRs, marker tokens (2956, 2998-3003, 3937), and a 48 B token plus a node copy for every replayed token.

**Directives**
* Dispatched by a `strcmp` chain (3182-3345).
* `#if` expressions are parsed into nodes and evaluated (`eval_expr` 3521ff).
* **Skipped groups are fully tokenized and discarded** (`skip_nl` 2384-2390, called from 3810-3813, 3184-3186, 3237-3242, 3258-3259).
* Repeated includes: there is no multiple-include guard skip, only a `#pragma once` list (2369-2370).

**Hand-off to the parser**
* `pre_out` (3986-4044) → `pptoken2token`: drops whitespace (1964-1965), maps identifiers to keywords, and converts numbers into nodes.
* String-literal concatenation is two code compares against the last recorded token (3997-3998).
* `VARR_PUSH recorded_tokens` (4043). **The whole TU is preprocessed before parsing starts** (`pre`, then `parse`, 21936-21941).

**Parser**
* Recursive descent over the `recorded_tokens` pointer array.
* `read_token` is one array read plus an increment (4106-4111); `match` is 4250-4259.
* Backtracking resets the index (`record_start`/`record_stop` 4113-4129; `TRY` 4266-4283).
* Expressions: `left_op`/`right_op` (4464-4524). There are no explicit stacks and no containers.
* `primary_expr` returns the token's pre-built leaf node (4308).
* At top level the parser tries `TRY(declaration)` first and on failure re-parses the header as a function definition (5701-5705).

**Nodes**
* `struct node` (689-706) is 48 B: code, uid, attr, op_link, a 16 B union. Allocated by `reg_malloc` (757-766).
* Positions live in a side VARR indexed by uid (`node_positions`, 708-717).
* The parser builds c2mir `node_t` directly. `do_context` annotates the same tree in place.

**Typedef-name disambiguation**
* A `tpname` HTAB keyed by (interned name pointer, scope node), holding only a typedef bit (4153-4192).
* Every declarator is registered with its typedef flag (4610-4620), and so are parameters (5719-5728).
* `typedef_name` walks the scope chain (5272-5286); `declaration_specs` uses it (4837-4838).
* Full symbol resolution happens later, in the checker (`symbol_find` 5969-5980; HTAB on (mode, id, scope)).

## 3. Q3: preprocessor + lexer Ir per token

Commands:
* Header stub: `hdrs.c`.
* Preprocessed input: `gcc -E -P AW -o aw_pp.c`, then `--toggle-collect=processing` (c2m `-fsyntax-only`) and `--toggle-collect='Program::tokenize(char const*)'` (madc).
* Wrapper: `aw_wrap.c` = `#define REQUESTS 1` / `#define SMAUG 1` / `#include "act_wiz.c"`.

| input | c2m Ir/token (parse tokens) | madc Ir/token |
|---|---:|---:|
| act_wiz, everything (c2m: glibc headers; madc: `-D`, embedded headers live) | **3,634** (389.5M/107,188); 2,151 per pp token | 2,841 (302.7M/106,570) |
| act_wiz, user code only (minus header stub) | **1,882** (184.2M/97,865); lexer function alone 1,263 (123.6M) | 2,211 (216.3M/97,844); pack-bound wrapper 2,385 (233.4M/97,851, pack bind included) |
| act_wiz, pure lexing (gcc -E -P input) | 1,547 (175.4M/113,353) | 1,665 (177.4M/106,549) |
| sqlite3.c | **3,548** (2,387.8M/673,070) | 2,680 (1,826.2M/681,521) |
| headers only (`hdrs.c`) | 22,020 (205.3M/9,323) | 9,901 (86.4M/8,726, madc's embedded headers) |

Where c2m's lexer Ir go on act_wiz (`--toggle-collect=get_next_pptoken_1`, 309.4M):

| | Ir | share |
|---|---:|---:|
| char layer: `cs_get` 136.7 + `getc` 24.4 + `t_getc`/`top_level_getc` 4.8 | 165.9M | 54% |
| scanner, self | 56.7M | 18% |
| interning: `str_add`, both `str_hash`es, `memcmp`, `strlen`, `str_eq` | 46.8M | 15% |
| token + node alloc: `reg_malloc`, `new_token*`, `new_id_token`, `set_node_pos` | 30.2M | 10% |

madc's char layer (`Source::get`/`get_slow`/`peek`/`peek_slow`, self) is 35.3M on the same file.

## 4. Q4: c2m techniques madc lacks, ranked by measured madc cost

act_wiz figures are pack-bound (wrapper) unless marked. Each row has a cheap proving experiment.

| # | technique (c2m) | why cheaper | madc counterpart + measured cost | fits? | proving experiment |
|---|---|---|---|---|---|
| 1 | **Type kinds are enums.** `struct type.mode` + `basic_type` (c2mir.c:273, 334, 346-370); `arithmetic_conversion` / `integer_promotion` switch on them (6381, 6406ff) | one compare per test | `usual_arithmetic_result` (tokens.h:691-729) calls `integer_conversion_rank` 3× (656-659). Each rank call runs `dd_is_platform_integer` = **4 failing `dynamic_cast`s** (datadef.h:2120-2126). On Linux LP64 those subclasses are never created (parser.cpp:20946-21012). Cost: **64.6M Ir act_wiz (3.7% of compile), 650.7M sqlite3 (4.2%)**; 2,053,888 `dynamic_cast`s on sqlite3. All `__dynamic_cast`: 94.7M / 843.5M (5.4% / 5.5%), and its `strcmp` of typeinfo names is most of the parse-window `strcmp` (15.7M) | Yes; no provenance impact; matches the enum-over-strings law | One line: an early `return false` in `dd_is_platform_integer` when `!target_llp64() && !target_int64_is_longlong()`, or a "minted" flag set by the 4 lazy constructors. Callgrind act_wiz + sqlite3; compare `usual_arithmetic_result`, `__dynamic_cast`, `Program::parse`, `translate_module` |
| 2 | **Intern once at lex; every later test is a pointer compare.** `new_id_token` (1372-1379); `macro_eq` (2205-2207); `tpname_eq` (4153-4155) | no hashing or `strlen` after lexing | `parseExpr_identifierArm` passes `ident_tb->spelling()` (`const char*`) into `const istring&` parameters, which re-interns: parser.cpp:41830 (unconditional, every identifier), 41880-41881, 42009-42011, ... `istring(char const*)` costs **47.9M act_wiz (2.7%), 320.0M sqlite3 (2.1%)**; the identifier arm's share is 31.2M / 218.4M (1,174,801 re-interns on sqlite3). The memo already exists: `TokenIdent::spelling_name()` (tokens.h:1669-1674) | Yes | Change those call sites to `spelling_name()`; callgrind `Program::parse` and the `istring(char const*)` call count |
| 3 | **Allocation-free recursive-descent expressions with no lookahead counting.** `left_op`/`right_op` (4464-4524) | no container and no heap per expression; arguments parsed as they come | `parseExpression` declares two `std::stack<TokenBase*>` (deque-backed) per call (parser.cpp:46376-46377): **37,932 deque map inits** (7.1M plus malloc/free, about 15M). `count_queued_call_arguments` (parser.cpp:32926): 18,737 calls, 14.8M. `delim_scan_step` lookahead: 16.9M. Together about 45M, **~12% of madc parse** | Yes (stacks); the lookahead is a design question | `typedef std::stack<TokenBase*, std::vector<TokenBase*>>` for both stacks and the helpers' signatures; callgrind `Program::parse`. Separately, inclusive Ir of `count_queued_call_arguments` with its callers (already 14.8M) |
| 4 | **The token cursor is an array index.** `read_token` (4106-4111); backtrack = reset index (4113-4129, 4266-4283) | ~5 Ir per token | `Program::nextToken` (madc.h:8282-8314): deque `pop_front`, `read_count`/`_tok_reread`/`_tok_max_reads` stats, `ParsePosition::set_from`, the pragma-pack check. 11.2M + `peekToken` 4.9M self = **16.1M (4.4% of parse)**; 128,307 consumes, ~87 Ir each | Yes | Compile the per-token stats out behind a macro (keep `_tok_consumed`); callgrind `Program::parse` |
| 5 | **The keyword/class is a field of the intern entry.** `kw_add` (5749-5751); `pptoken2token` (1955-1962) | one lookup gives the class | Per-token string classification: `compound_type_specifier_flag` 6.7M + `lexer_type_token` 4.0M + `madc_slot_id_for` 4.9M inclusive = **15.6M (5.2% of the `-D` lex window)**; 12.8M self, 7.2% of the 177.4M pure-lex run | Yes (keyed by `spelling_id`) | Memoize one of them in a `vector<uint8_t>` indexed by `spelling_id`; callgrind tokenize on `aw_pp.c` (pure lex, 177.4M baseline) |
| 6 | **Macro bodies stored as token arrays, replayed** (2114-2119, 2491-2500, 2564-2580, 2939-3018) | no re-lex, except for `##` | Body kept as text (`MacroDef::body`, madc.h:5856-5879); substituted and re-lexed (lexer.cpp:10407, 10426-10427). **Ceiling ≤ ~50M Ir** (≈20% of pack-bound lex, ~5-6 ms): pack-bound tokenize 233.4M − pure lex 177.4M, which still includes ~5.6M of zstd pack bind | Yes, with tokens kept (a copy can carry both the invocation and the definition token). **But c2m's own replay costs ≥49M on act_wiz** (`define` 25.5 + `try_param_macro_call` 10.2 + `process_replacement` 9.4 + `macro_hash` 3.8): its per-replay token copies and mallocs eat the gain. Copy tcc's scratch-buffer replay, not c2m's | Done for free: the `aw_pp.c` differential. Next, an env-gated counter of bytes lexed from pushback frames, multiplied by madc's measured ~321 Ir/byte |
| 7 | **No AST→IR translation pass.** The parser builds `node_t` directly (`new_pos_node*` 780-817) | one tree walk fewer | `CirBuilder::translate_module`: **195.3M (11.1% of compile), ~2,000 Ir/token**; 1,456M on sqlite3 | No as-is: MC11-IR keeps cir_node with attached tokens. What fits is making the walk cheaper: its top costs are RTTI again (`usual_arithmetic_result` 28.9M inside the CIR window) and `std::set<istring>` ordered by `memcmp` (`cir_harvest_type_refs`, cir_builder.cpp:35623: 27.5M act_wiz / 219.3M sqlite3) | After #1, re-measure; `cir_harvest_type_refs` with an id-keyed `unordered_set` |

Already adopted in madc (no gain available):
* Tokens are the leaf AST nodes (c2m pre-builds a node per token).
* Bump allocation of tokens (`TokenBase::operator new` 6.6M for about 107k tokens).
* The whole TU is buffered before parsing; c2m is not pull-based either.
* Positions are kept on tokens.

Does not fit:
* c2m's typedef-bit-only parser table with name resolution deferred to the checker (4153-4192, 5272-5286; checker `symbol_find` 5969). madc's C++ parse needs real lookup at parse time. Measured lookup share: `Program::findVariable` 22.6M + `resolve_preferred_identifier` 21.5M inclusive (overlapping), about 6-10% of parse.

## 5. Q5: places c2m is slower than madc (do not copy)

1. **Char layer.**
   * What it does: `fgetc` per char → line VARR → trigraph pass → reverse → pop (1089-1140); the top-level file is slurped by `getc` per byte and then re-read through a function pointer per char (driver 928, 472-475).
   * c2m cost: **94 Ir/char** (`cs_get` 168.6M over 1,792,577 calls) on act_wiz; 1,119M on sqlite3, plus 211M for the driver slurp.
   * madc: about 35M on act_wiz.
2. **Skipped `#if` groups are fully tokenized.**
   * What it does: every skipped token gets a 48 B object, its text interned, and an `N_ID` node for identifiers (2384-2390, 3810-3813).
   * c2m cost: `skip_nl` 94.1M on act_wiz (92.5M of it in glibc headers; 24% of `processing`), 716M on sqlite3.
   * madc: `skipConditionalBlock` + `PpGroupScan` 3.4M inclusive (lexer.cpp:10784).
3. **No include-guard skip** (only `#pragma once`, 2369-2370).
   * c2m cost: a second `#include <stdio.h>` costs **+6.86M Ir** (`s1.c` vs `s2.c`, `-fsyntax-only`: 49.98M → 56.84M); the 34,649-byte file is re-read and re-tokenized.
   * madc: +0 (same 1,904 tokens; guard rule at lexer.cpp:7477-7525).
4. **Each identifier is hashed three times.**
   * Where: at lex (1372-1374), at the macro lookup (`strlen` + `mir_hash` in `macro_hash`, 2209-2211; 52,530 lookups, 3.8M on act_wiz; 284,258, 21.0M on sqlite3), and at the keyword lookup (1956).
   * madc folds the hash into the scan and probes macros by sid.
5. **Comment and whitespace text is interned** as token text (1391-1449, `new_token_wo_uniq_repr` 917-920).
6. **Per-macro-call mallocs**, plus a 48 B token and a node copy for every replayed token (2272-2281, 2760-2797, 910-915). This is why c2m's replay is not cheaper than madc's re-lex (§4 #6).
7. **Directive dispatch is a `strcmp` chain** (3182-3345); `##` re-lexes (2840-2856). Both minor.
8. **Function headers are parsed twice at top level** (5701-5705). Minor: c2m's whole parse is only 449 Ir/token on user code.

## 6. Caveats

* **Header attribution.** The header-only split assumes act_wiz's headers cost what the stub's do; the stub uses the same 14 includes in the same order. The two compilers lex *different* header text: glibc for c2m, madc's embedded copies for madc. Only the user-code and preprocessed-input rows are like-for-like.
* **Call counts.** `--toggle-collect` profiles report whole-run call counts, so call counts above come from the untoggled profiles; only Ir are taken from the toggled ones.
* **ms estimates.** Rows marked "≈" are estimates. The pre / parse / check / gen / link ms are c2m's own measured timestamps.
* **Wall noise.** madc's absolute ms vary ±15% between batches; the interleaved runs are the ones to quote.

## 7. Files (container `/tmp/tsprobe/c2mrecon`)

* Inputs: `hdrs.c`, `aw_pp.c`, `aw_wrap.c`, `s1.c` / `s2.c`.
* c2m profiles: `cg_aw.out`, `cg_h.out`, `cg_sq.out`, `cg_c2m_lex.out`, `cg_c2m_pp.out`.
* madc profiles: `cg_madc_aw.out`, `cg_madc_wrap.out`, `cg_madc_h.out`, `cg_madc_sq.out`, `cg_madc_{parse,cir,lex,pp}.out`.
* `callers.py`: resolves a callgrind caller through PLT stubs.
