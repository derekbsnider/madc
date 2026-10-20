# madc front end: parse and CIR build cost recon (2026-10-20)

Container `ssh -p 2299 dev@localhost`, `bin/madc-release` + `lib/release/libmadc.so` (-O2, unstripped, pack attached), tree @ d11b136c7.
Profiles and scripts: `/tmp/tsprobe/fe/` on the container. Nothing in any repo was edited or built.

## 0. Method

* Phase-only profiles (one run each, all four in parallel; script `/tmp/tsprobe/fe/run4.sh`):
  * Parse: `valgrind --tool=callgrind --toggle-collect='Program::parse(TokenProgram*)' ...`
  * CIR build: `valgrind --tool=callgrind --toggle-collect='CirBuilder::translate_module(Program*)' ...`
  * act_wiz flags: `-DREQUESTS -DSMAUG -I.../smaug1.8/src -c`. sqlite3 flags: `-c scripts/parse_cost/sqlite3.c`.
  * Entry points come from `nm -C lib/release/libmadc.so`. They match the `--show-stats` timers at madc.cpp:1875 and madc_cir.cpp:1036-1053.
* Overlap probes (`/tmp/tsprobe/fe/run_c2.sh`) collected only inside:
  * the parse-side class-operator machinery;
  * the CIR-side class-operator machinery;
  * `usual_arithmetic_result`;
  * `parseExpr_identifierArm`.
* Analysis scripts:
  * `/tmp/tsprobe/fe/cgq.py FILE self|callers|callees|calls`: self cost, call arcs (inclusive Ir plus call count), callers resolved through PLT stubs, recursion `'N` folded.
  * `/tmp/tsprobe/fe/cgcat2.py`: categories keyed on the function NAME. The original `cg_categories.sh` put madc functions taking `std::stack&` into "std::".
* Caveats:
  1. **Call counts in toggled profiles are whole-run counts** (verified: `usual_arithmetic_result→__dynamic_cast` is 205,488 calls in the parse profile, the CIR profile and the whole-compile profile alike). Ir is per phase. Counts below are therefore per compile.
  2. libmadc's own objects have no DWARF (only `mir.c`, `mir-gen.c`, `c2mir.c` have CUs; checked with `readelf --debug-dump=info`). madc attribution is per function, and file:line comes from reading the source.
  3. Inclusive numbers are taken from the outermost (non-`'2`) entry, the one callgrind_annotate accounts correctly.

## 1. Phase totals

| | act_wiz.c | sqlite3.c |
|---|---|---|
| parse Ir | **386.2M** (wall 48-52 ms) | **2,713.4M** (wall 0.39 s) |
| CIR build Ir | **190.4M** (wall 29-31 ms) | **1,456.0M** (wall 0.25 s) |
| whole compile Ir | 1,834.5M | n/a (release) |
| tokens produced / consumed | 106,570 / 128,307 | 681,521 / 750,781 |

* Throughput: about 7.7M Ir/ms in parse and 6.3M Ir/ms in CIR (act_wiz).
* act_wiz has no pack container ("live parse"). Even so, parseFunction is 90.3% of parse inclusive: the headers are cheap here and the bodies are the cost.

## 2. Category split (self Ir, `cgcat2.py`)

| category | aw parse | aw CIR | sq parse | sq CIR |
|---|---|---|---|---|
| madc own code | 48.8% | 45.7% | 49.8% | 43.0% |
| name interning (istring_table / intern_table) | 11.5% | 2.5% | 10.4% | 2.0% |
| RTTI (`__dynamic_cast`, `__do_dyncast`) | 9.5% | 15.1% | 11.3% | 17.4% |
| libc strcmp (≈75% parse / ≈100% CIR from typeinfo compares inside dynamic_cast) | 5.8% | 6.4% | 6.8% | 9.0% |
| libc memcmp/strlen/memcpy | 7.9% | 9.1% | 7.6% | 8.7% |
| C++ std:: (containers, string ==) | 6.7% | 7.3% | 6.2% | 7.9% |
| malloc/free + operator new/delete | 7.3% | 2.4% | 5.9% | 1.7% |
| TLS `__tls_get_addr` | 1.9% | 0.0% | 1.5% | 0.0% |
| C back end (c2mir node/string calls) | – | 10.2% | – | 9.4% |
| **overhead -O2 cannot remove (sum of the non-madc, non-c2mir rows)** | **51.0%** | **44.1%** | **50.0%** | **47.7%** |

* **RTTI total (dynamic_cast plus its strcmp):**
  * act_wiz: parse 53.5M (13.9%), CIR 41.7M (21.9%).
  * sqlite: parse 451.8M (16.7%), CIR 391.6M (26.9%).
  * One failed `dynamic_cast` costs about 289 Ir: 59.4M over 205,488 calls. Each step up the hierarchy is a `strcmp` of mangled names, because libstdc++ type_info equality falls back to `strcmp`.
* "madc own code" overstates the real parsing work. It contains the C++ class-operator, UFCS and eval-scope machinery executed on C input (C2, C6 and C4 below).

## 3. Top 25 SELF functions

Columns: self Ir, % of phase, whole-run calls. The full lists for all four profiles are in `top25.txt` next to this file.

**act_wiz parse (386.2M):**

| # | function | self Ir | % | calls |
|---|---|---|---|---|
| 1 | istring_table::lookup_or_insert | 24.8M | 6.42 | 275K |
| 2 | strcmp | 22.4M | 5.80 | 1.35M |
| 3 | memcmp | 18.7M | 4.84 | 1.75M |
| 4 | __dynamic_cast | 16.3M | 4.21 | 383K |
| 5 | __si_class_type_info::__do_dyncast | 12.5M | 3.23 | |
| 6 | Program::nextToken | 11.2M | 2.90 | 128K, 87 Ir/token |
| 7 | istring::istring(const char*) | 10.2M | 2.65 | 253K |
| 8 | strlen | 9.5M | 2.46 | 1.67M |
| 9 | parseExpr_identifierArm | 9.2M | 2.38 | |
| 10 | TokenCpnd::findVariableThisScope | 7.7M | 2.00 | |
| 11 | __tls_get_addr | 7.5M | 1.94 | 838K |
| 12 | malloc | 7.5M | 1.93 | |
| 13 | _int_malloc | 7.2M | 1.85 | |
| 14 | TokenIdent::spelling | 7.1M | 1.84 | 509K |
| 15 | parseExpression | 7.0M | 1.83 | |
| 16 | delim_scan_step<TokenStream> | 6.8M | 1.76 | |
| 17 | _int_free | 6.4M | 1.66 | |
| 18 | count_queued_call_arguments | 5.8M | 1.49 | |
| 19 | std::operator==(string, const char*) | 5.5M | 1.43 | 858K |
| 20 | intern_table::intern | 5.5M | 1.42 | |
| 21 | peekToken | 4.9M | 1.26 | |
| 22 | parseExpr_operatorArm | 4.6M | 1.18 | |
| 23 | __class_type_info::__do_dyncast | 4.5M | 1.15 | |
| 24 | Program::findVariable | 4.4M | 1.13 | |
| 25 | TokenIdent::spelling_name | 3.7M | 0.97 | |

**act_wiz CIR (190.4M):**

| # | function | self Ir | % |
|---|---|---|---|
| 1 | memcmp | 12.3M | 6.49 |
| 2 | strcmp | 12.2M | 6.43 |
| 3 | __dynamic_cast | 12.0M | 6.30 |
| 4 | CirBuilder::make (130K nodes) | 11.1M | 5.82 |
| 5 | __si_class_type_info::__do_dyncast | 9.5M | 4.99 |
| 6 | translate_expr | 8.2M | 4.31 |
| 7 | std::set<istring> _M_get_insert_unique_pos | 7.0M | 3.66 |
| 8 | cir_harvest_type_refs | 6.5M | 3.43 |
| 9 | c2mir_next_uid | 4.4M | 2.33 |
| 10 | str_add | 3.4M | 1.77 |
| 11 | __class_type_info::__do_dyncast | 3.4M | 1.76 |
| 12 | c2mir_op_append | 3.3M | 1.73 |
| 13 | strlen | 3.2M | 1.70 |
| 14 | lookup_or_insert | 2.9M | 1.52 |
| 15 | madc_slot_id_for | 2.9M | 1.51 |
| 16 | set_node_pos | 2.7M | 1.40 |
| 17 | std::operator==(string, const char*) | 2.4M | 1.28 |
| 18 | __vmi_class_type_info::__do_dyncast | 2.4M | 1.24 |
| 19 | cir_parameter_scope_tags | 2.0M | 1.05 |
| 20 | str_hash | 1.9M | 1.02 |
| 21 | build_call_args | 1.6M | 0.83 |
| 22 | usual_arithmetic_result | 1.5M | 0.81 |
| 23 | member_node | 1.5M | 0.77 |
| 24 | istring(const char*) | 1.4M | 0.74 |
| 25 | std::set<istring>::find | 1.3M | 0.70 |

**sqlite parse (2,713M):**

| # | function | self Ir | % |
|---|---|---|---|
| 1 | strcmp | 184.0M | 6.78 |
| 2 | lookup_or_insert | 158.6M | 5.85 |
| 3 | __dynamic_cast | 132.1M | 4.87 |
| 4 | __do_dyncast (si) | 107.4M | 3.96 |
| 5 | memcmp | 106.9M | 3.94 |
| 6 | delim_scan_step | 100.2M | 3.69 |
| 7 | strlen | 74.9M | 2.76 |
| 8 | istring(const char*) | 68.5M | 2.52 |
| 9 | nextToken | 65.3M | 2.41 |
| 10 | identifierArm | 63.5M | 2.34 |
| 11 | parseExpression | 53.8M | 1.98 |
| 12 | string==const char* | 53.8M | 1.98 |
| 13 | TokenIdent::spelling | 49.1M | 1.81 |
| 14 | malloc | 43.1M | 1.59 |
| 15 | _int_free | 41.9M | 1.54 |
| 16 | __tls_get_addr | 40.8M | 1.50 |
| 17 | DelimDepth::update | 38.8M | 1.43 |
| 18 | __do_dyncast (class) | 38.1M | 1.41 |
| 19 | operatorArm | 37.7M | 1.39 |
| 20 | findVariableThisScope | 36.6M | 1.35 |
| 21 | resolve_declared_type_token | 34.3M | 1.26 |
| 22 | intern_table::intern | 32.0M | 1.18 |
| 23 | peekToken | 31.1M | 1.15 |
| 24 | spelling_name | 30.6M | 1.13 |
| 25 | is_contextual_identifier_token | 29.5M | 1.09 |

**sqlite CIR (1,456M):**

| # | function | self Ir | % |
|---|---|---|---|
| 1 | strcmp | 130.7M | 8.98 |
| 2 | __dynamic_cast | 110.3M | 7.58 |
| 3 | __do_dyncast (si) | 97.0M | 6.66 |
| 4 | CirBuilder::make (980K nodes) | 83.3M | 5.72 |
| 5 | memcmp | 83.3M | 5.72 |
| 6 | set<istring> insert | 65.3M | 4.48 |
| 7 | translate_expr | 59.7M | 4.10 |
| 8 | cir_harvest_type_refs | 48.6M | 3.34 |
| 9 | __do_dyncast (class) | 34.1M | 2.34 |
| 10 | c2mir_next_uid | 33.3M | 2.29 |
| 11 | strlen | 32.4M | 2.23 |
| 12 | string==const char* | 29.6M | 2.03 |
| 13 | c2mir_op_append | 24.7M | 1.70 |
| 14 | madc_slot_id_for | 21.2M | 1.46 |
| 15 | set_node_pos | 20.7M | 1.42 |
| 16 | str_add | 20.7M | 1.42 |
| 17 | lookup_or_insert | 18.3M | 1.26 |
| 18 | usual_arithmetic_result | 17.9M | 1.23 |
| 19 | cir_parameter_scope_tags | 14.0M | 0.96 |
| 20 | str_hash | 11.2M | 0.77 |
| 21 | integer_promoted_type | 11.1M | 0.76 |
| 22 | promoted_operand_type | 11.1M | 0.76 |
| 23 | istring(const char*) | 10.3M | 0.71 |
| 24 | translate_stmt | 9.9M | 0.68 |
| 25 | resolve_builtin_type_spelling | 9.6M | 0.66 |

## 4. Culprits, ranked by measured Ir on act_wiz (parse + CIR = 576.6M)

### C2. The C++ class-operator / carrier machinery runs on every operator of a C program

**act_wiz: 78.4M inclusive, 13.6% of parse+CIR.** Of that, 48.0M is C1 running underneath it; **30.3M is exclusive of C1**.

* Measured by toggle on the entry points (`run_c2.sh`):
  * parse 48.8M (12.6%): `popOperator` → `Program::resolve_object_operator_type` (30.0M, parser.cpp:22348) and `Program::lower_free_operator_to_call` (18.8M, parser.cpp:22478, which calls `operator_function_operand`, 22175).
  * CIR 29.5M (15.5%): `translate_expr` calls three functions:
    * `CirBuilder::class_operator_call` 16.9M (cir_builder.cpp:23267);
    * `upcast_class_ptr` 6.7M (cir_builder.cpp:7069);
    * `carrier_builtin_operator_refusal` 5.9M (cir_builder.cpp:21352). This one belongs to the dialect carrier ddARRAY.
  * 31.5M of the parse 48.8M and 16.5M of the CIR 29.5M is `usual_arithmetic_result`.
* sqlite inclusive: 464.3M parse (17.1%) + 356.0M CIR (24.4%) = 820M. The C1 overlap was not measured there.
* Mechanism. Every binary operator calls `operand_object_class(left/right)`, at parser.cpp:22151 and cir_builder.cpp:14198, 2-4 times. Each call runs `array_operand_type` (parser.cpp:22223, building a `vector<carray_dim_t>`) and then `operand->datadef()` again.
  * `datadef()` on an operator node is not cached. It recomputes its subtree through `promoted_operand_type` → `usual_arithmetic_result` (tokens.h:691), about 3 datadef evaluations per operator node.
  * In a C TU no class object can exist, so all of it answers NULL.
  * `upcast_class_ptr` computes `operand_value_type(rhs)` (42.0M sq) and `expr_pointee_class` (43.5M sq) before checking that `lhs` is a class pointer at all.
* Cheaper shape:
  * Return NULL from both `operand_object_class` when `!presents_as_cpp()`. That owner exists: madc.h:6871, read from the LanguageStd enum, the sanctioned gate (I2/I8). Keep `resolve_object_operator_type`'s arms that matter in C (SIMD compare, unary-plus and additive array decay).
  * In `upcast_class_ptr`, compute `base = pointee_user_class(lhs_dd)` first and return early.
  * Second step, any std: memoize the operator's computed type on the token once its operands are final, so `datadef()` stops being O(subtree) per query.
* Experiment: add the two early returns plus the reorder, rebuild the release, and `cmp` the objects against `aw.o` and `sq.o` (expect identical). Then callgrind both phases (expect about -78M act_wiz, -800M sqlite) and run the c-testsuite and c-torture lanes.

### C1. Four `dynamic_cast`s for a platform-integer test that is always false on Linux

**act_wiz: 64.6M** (`usual_arithmetic_result` inclusive, toggle probe; 59.4M of it is `__dynamic_cast`, 205,488 casts at about 289 Ir each). That is parse 30.5M (7.9%) + CIR 28.9M (15.2%).

* sqlite: 271.6M parse (10.0%) + 322.0M CIR (22.1%) = **593.6M** (2.05M casts).
* Site:
  * `usual_arithmetic_result` (tokens.h:691-729) calls `integer_conversion_rank` (tokens.h:656) up to 4×.
  * That calls `dd_is_platform_integer` (datadef.h:2120-2126): **4 dynamic_casts**.
  * `unsigned_integer_counterpart` (tokens.h:665-676) adds 2 more.
  * Result: about 15 failed casts per call.
* On LP64 Linux the four `DataDefPlatform*` classes are never instantiated: `dd_platform_long()` and its siblings return `ddINT64`/`ddUINT64` (parser.cpp:20946-21022; comment at datadef.h:2096-2098).
* Cheaper shape, through the existing owner: the DataDef kind-accessor family (datadef.h:531-569, "the O(1) replacement for dynamic_cast"). Add `virtual bool is_platform_integer() const`, or a ctor-set flag on the four classes, and test `ld == rd` before ranking. Replace `unsigned_integer_counterpart`'s casts with the same accessor.
* Experiment: rebuild and callgrind `--toggle-collect='usual_arithmetic_result(DataDef*, DataDef*)'`; expect 64.6M → about 5M on act_wiz. `cmp` the objects.
* Zero-code bound: the toggle probe itself (64.6M, of which 59.4M in `__dynamic_cast`).

### C3. The identifier arm re-interns the same spelling about 9 times and strcmp's it about 12 times per identifier

**act_wiz parse: 42.9M (11.1%)** = 31.2M re-intern + 11.7M `spelling_is`.

* sqlite parse: 218.4M + 83.8M = **302.2M (11.1%)**.
* Site: `Program::parseExpr_identifierArm` (parser.cpp:41811). 18,427 invocations on act_wiz; 166,169 `istring(const char*)` constructions; 220,994 `spelling_is` calls.
  * It passes `ident_tb->spelling()`, a `const char*` (tokens.h:1666), to about 25 `const istring&` parameters: parser.cpp:41830, 41862, 41880-41881, 41906, 41910, 42000-42003, 42009, 42028-42035, 42057, 42164.
  * Each pass is an implicit `istring(const char*)` (istring.h:216): strlen, then hash, then probe, then memcmp, about 159 Ir.
  * `spelling_is("operator")` / `"__FUNCTION__"` / `"__func__"` / `"__PRETTY_FUNCTION__"` (41842, 41854-41855) is a `strcmp` each (tokens.h:1675).
  * The predicates then byte-compare `name == "literal"` through `std::string==const char*`: parser.cpp:2951-3000 (`is_alignof_identifier`, `is_type_query_identifier`, `is_bool_literal_identifier`, `is_nullptr_identifier`), 3180-3188, 33535.
  * A `TokenIdent contextual_ident(contextual_name)` is built on the stack for every identifier (41823-41827; ctor 4.3M, it interns into strpool), even when `tb` already is a TokenIdent.
* Existing owners:
  * `TokenIdent::spelling_name()` (tokens.h:1671): "the pool's memo ... a parser lookup or compare never re-interns". It is not used here.
  * `MADC_ISTRING_LITERAL` (istring.h:339).
  * Precedents for identity compares: `type_trait_of` (parser.cpp:16649-16653, "Interned once: each test is an identity compare") and `token_is_operator_id_start` (parser.cpp:5436-5442).
* Cheaper shape:
  * One `const istring name = ident_tb->spelling_name();` at the top, passed everywhere.
  * Predicates compare interned rows by pointer.
  * Build `contextual_ident` only when `tb` is not an identifier.
* Experiment: count `istring::istring(char const*)` calls whose caller is identifierArm (`cgq.py ... callers`). Expect 166K → about 0, -31M Ir, and -11M more once `spelling_is` becomes identity compares.

### C4. Other hot `dynamic_cast`s where a kind accessor already exists

**act_wiz: about 31.9M.** Total RTTI is 95.2M (parse 53.5M + CIR 41.7M), less C1's 59.4M, less the casts counted in C2 and C6.

| site | file:line | casts | Ir |
|---|---|---|---|
| `update_pointer_object_size_hints` (3 casts per call, recursive) and `known_object_size_for_expr` (up to 9) | parser.cpp:18565-18592 and 18485-18563 | 19,307 and 4,088 | 5.8M and 0.9M |
| `CirBuilder::object_returning_call_class` | cir_builder.cpp:6571-6573 | 19,856 | 5.4M |
| `free_callee_arity_inviable` (`dynamic_cast<FuncDef*>`) | parser.cpp:41576 | | 3.0M |
| `parse_declaration_body` | | 12,102 | 2.9M |
| `is_constant_evaluated_call` | cir_builder.cpp:14511 | | 1.5M |
| `member_access_violation` (C++ access check, runs in C) | parser.cpp:35383 | | 1.2M |
| `is_class_object_value` | | | 1.1M |
| `require_complete_class` | parser.cpp:13827 | | 1.0M |
| `arrow_operator_call` | | | 1.0M |
| ... | | | |

sqlite: about 250M (843M total RTTI less C1).

* Owners: the token accessors `as_assign_tok`, `as_terq_tok`, `as_var_tok`, `as_member_tok`, `as_addr_of_tok`, `as_addr_expr_tok`, `as_cast_tok`, `as_callfunc_tok`, `as_objtemp_tok` (tokens.h:403-470); `id()==tkComma`; `as_funcdef_dd()` (datadef.h:555).
* Experiment: the whole-compile callers of `__dynamic_cast` (`cgq.py /tmp/tsprobe/cg.aw.rel.out callers __dynamic_cast`) list each site with its count. Convert the top 5 and re-run; expect `__dynamic_cast` calls 383K → under 60K.

### C5. Every CIR module filter re-interns c2mir strings into byte-ordered `std::set<istring>`

**act_wiz CIR: 30.3M (15.9%)** = `cir_harvest_type_refs` 27.5M + `cir_parameter_scope_tags` 2.8M.

* sqlite CIR: 219.3M + 18.9M = **238.2M (16.4%)**.
* Site: cir_builder.cpp:35623-35646 (`ids.insert(n->u.s.s)`, `tags.insert(...)`) and 35653-35670; driven at 38264-38320 and 35676.
  * Every N_ID's `const char*` is re-interned (`istring(const char*)`: 6.3M aw / 45.5M sq).
  * It is then inserted into `std::set<istring>`, whose `operator<` compares bytes (istring.h `operator<`): 12.2M aw / 108.5M sq for the inserts, about 600 Ir per insert in sqlite.
* Cheaper shape: N_ID strings are already unique pointers from `c2mir_uniq_str` (`CirBuilder::id`, cir_builder.cpp:248-255). Key the sets on that pointer in a `std::unordered_set<const char*>`, or sort-unique a vector. The admission fixpoint (38320-38360) only tests membership, so order does not matter.
* Experiment: callgrind `--toggle-collect` on `cir_harvest_type_refs` (27.5M aw today) after the swap, plus `cmp` the objects.

### C6. Every call eagerly scans its argument list for a dialect-only UFCS check

**act_wiz parse: 18.3M (4.7%)** = `count_queued_call_arguments` 14.8M (18,737 calls; a `DelimDepth` walk over the whole argument run, 8.3M of it in `delim_scan_step`) + `free_callee_arity_inviable` 3.5M (it builds a `namespace + "::" + name` std::string per call, plus a dynamic_cast).

* sqlite: 63.1M + 26.1M = 89.2M.
* Site: parser.cpp:43668-43673. The `count_queued_call_arguments()` (parser.cpp:32926) argument is evaluated before `free_callee_arity_inviable` (41574) has checked that any template or overload candidates exist. Its consumer, `ufcs_call_fallback`, is `--std=madc` only (comment at 43660-43667).
* Cheaper shape: gate on `dialect_surface_enabled()` (madc.h:6880, the one LanguageStd owner), and count arguments lazily, only when a candidate set exists.
* Experiment: count_queued_call_arguments calls 18,737 → 0 on a C TU; `cmp` the objects.

### C7. Literal byte-compare ladders outside the identifier arm

**act_wiz: about 15M.** `resolve_builtin_type_spelling` costs 5.0M parse + 5.0M CIR; the runtime-eval-scope helpers cost about 4.6M.

* sqlite: 84.1M + 69.3M = 153.4M for `resolve_builtin_type_spelling`, plus 8.6M + 8.0M for eval scope.
* `integer_promoted_type` (parser.cpp:63817) calls `Program::resolve_builtin_type_spelling(MADC_ISTRING_LITERAL("int"))` on EVERY call (63821), before any test.
  * The table (parser.cpp:4088-4150) is a chain of about 20-60 `name == "literal"` std::string compares (strlen + memcmp each), and "int" is the 15th spelling tested.
  * The answer is the constant `&ddINT32`.
  * 288,962 calls in sqlite. Same at 63849 for `"unsigned int"`.
* `runtime_eval_scope_target` (parser.cpp:30781-30815) and `is_runtime_eval_scope_ctx_helper_name` (724-736) do 10 string compares per call on every C call. That is the madc:: eval runtime, a `--std=madc` surface.
* `std::operator==(string, const char*)`: 858,077 calls per act_wiz compile, 11.6M parse + 4.9M CIR inclusive.
* Cheaper shape:
  * `integer_promoted_type` returns `&ddINT32` / `&ddUINT32`, or holds them in a function-local static.
  * `resolve_builtin_type_spelling` keeps its single-owner role but looks up a static interned-row table (identity compare, the `type_trait_of` pattern).
  * The eval-scope helpers are gated on `dialect_surface_enabled()`.
* Experiment: `cgq.py calls 'std::operator=='`: 858K → under 100K; `resolve_builtin_type_spelling` inclusive 10M → about 0.5M.

### C8. Each identifier is looked up twice

**act_wiz parse: 13.1M (3.4%)** for `find_variable_for_contextual_type_name` (parser.cpp:5184), called on every identifier at 41905-41906.

* sqlite: 81.3M.
* The real resolution then repeats the lookup: `resolve_c_identifier` (5.4M) / `parseStatementBody` → `Program::findVariable`.
* Each `findVariable` (parser.cpp:30936) re-interns the istring into the per-Program `strpool` (30945; 2.7M aw), walks the scope chain, and then walks `Method::findParameter` (parser.cpp:24930), a linear `s.compare(name)` byte compare (5.3M aw incl).
* Cheaper shape: pass the Variable the contextual lookup found into the resolution. Cache the strpool sid on the istring entry, the FITS item in docs/plans/2026-10-20-tinycc-recon.md §2.2. `findParameter` compares istrings by pointer.
* Experiment: count `Program::findVariable(istring)` calls. Today there are 31,829 per act_wiz compile for 18,427 identifier-arm entries, and 18,427 of them (58%) are the contextual pre-check. The fix should remove those.

### C9. The `if` condition probe runs a template-argument scan to the next `>`: quadratic

**act_wiz: 8.5M** (49,272 `delim_scan_step` from `resolve_declared_type_token`). **sqlite: 213.8M (7.9% of parse; 1,234,530 steps, 1.6 steps per consumed token).**

* Site:
  * `TokenIF::parse` (parser.cpp:55392-55393) calls `resolve_declared_type_token(tn)` on the first token of every condition. It is probing for a C++ condition declaration; C has none.
  * Its statement-scope demand (parser.cpp:14583-14612) scans with a **bare `DelimDepth d;`** (no Program). That violates delimiter-tracking.md: a stream scan must be `DelimDepth d(this)`.
  * The scan is not gated on the name being a template, so `if (x < y)` opens a phantom angle. The walk then runs until some later `>` closes it, or to the 4,096-step guard.
* Measured on a synthetic file `/tmp/tsprobe/fe/ifscan_lt.c` (one `if (a < b)`, then 2,000 statements): **4,095 `delim_scan_step` calls, +0.81M parse Ir for ONE `if`**. The `a <= b` twin has 0 calls.
* sqlite has 659 `if( ident <` heads (`grep -cE`): about 1,870 steps each.
* Cheaper shape:
  * Gate the scan on `find_template(tname) || find_template_alias(tname)`, the context gate the expression path already uses (parser.cpp:41870-41881), and construct `DelimDepth d(this)`.
  * In C, skip the condition-declaration probe (`presents_as_cpp()`).
* Experiment: `ifscan_lt.c` / `ifscan_le.c` callgrind (commands in `/tmp/tsprobe/fe`). After the fix, `lt` should equal `le` (96.55M parse Ir).

### C10. `DBG()` reads a `thread_local` through `__tls_get_addr` in parser hot paths

**act_wiz parse: 7.5M self (1.9%)** for 837,618 calls, plus about 3M call overhead. sqlite: 40.8M (1.5%), 4.6M calls.

* Declaration and definition: `extern thread_local bool madc_verbose` (datadef.h:25), `DBG` (datadef.h:27); defined weak at madc_globals.cpp:25. That gives the global-dynamic TLS model in a `-fPIC` .so.
* Hot callers: `TokenCpnd::findVariable`, `DataDefSTRUCT::m_type`/`m_offset` (datadef.h:1661: DBG inside the member loop), `findParameter`, `popOperator`, `parseExpression`.
* Cheaper shape: `__attribute__((tls_model("initial-exec")))` on the ELF declaration (one %fs load, no call; libmadc is DT_NEEDED and the bools fit the static-TLS surplus for dlopen hosts), or a process-wide `madc_verbose_any` plain-bool pre-test in `DBG`.
* Experiment: rebuild and count `__tls_get_addr` calls in the parse profile: 837K → about 0.

### Smaller items, act_wiz parse+CIR

* **std::deque-backed `std::stack` per `parseExpression`** (parser.cpp:46376-46377, `stack<TokenBase*>` exStack/opStack): `_M_initialize_map` does 2 `operator new` per deque.
  * Cost: 7.1M aw (1.8%) and 51.2M sq (1.9%) on the allocation side, plus the matching frees. 247,696 constructions in sqlite.
  * There is also a full deque copy at 45350.
  * Fix: `std::stack<TokenBase*, std::vector<TokenBase*>>`, a typedef across the 43 `std::stack<TokenBase *>` occurrences in madc.h (18) and parser.cpp (25). Experiment: `_M_initialize_map` calls → 0.
* **Virtual `id()`/`type()`/`as_*`/`datadef()` leaf accessors**: 8.1M self in aw parse (2.66M calls), plus indirect-call overhead. Lexed tokens already carry the TokenID in `rec.kind` (tokens.h:214).
* **`c2mir_next_uid`** (c2mir.c:22111-22116) does a `VARR_PUSH` per node, about 41 Ir: 4.4M aw / 33.3M sq self. `set_node_pos` (c2mir.c:714-717) does the same grow-loop. Bulk-grow `node_positions` instead.
* **`Program::nextToken`** (madc.h:8282-8312): 87 Ir per token, 11.2M aw / 65.3M sq. It covers statistics counters, the six-static `ParsePosition::set_from`, a pragma-pack check, and `madc_token_for_slot` (the slot-id indirection, madc.h:3243-3263).
* **Intra-libmadc calls through PLT stubs**: `promoted_operand_type`, `array_operand_type`, `operand_value_type`, `spelling_is`, `c2mir_next_uid` and `madc_slot_id_for` are all reached via `0x49f…` stubs. `-fno-semantic-interposition` is set (Makefile:98), but cross-TU calls still bind through the PLT without `-Wl,-Bsymbolic-functions` or hidden visibility. The cost is small per call and is an owner call (MEMORY note).

## 5. What it adds up to

On act_wiz, the non-overlapping sum of C1-C10 plus the deque item:

| item | Ir |
|---|---|
| C1 + C2 | 94.9M |
| C3 | 42.9M |
| C4 | 31.9M |
| C5 | 30.3M |
| C6 | 18.3M |
| C7 | 15M |
| C8 | 13.1M |
| C9 | 8.5M |
| C10 | 7.5M |
| deque | 7.1M |
| **total** | **about 270M of 576.6M (≈47%)** |

* That is parse+CIR wall 77-83 ms → about 41-44 ms.
* On sqlite the same items are about 2.4G of 4.17G (≈57%), because the quadratic C9 and C1/C2 scale with expression density.
* This does **not** reach the 4× target (front end lex+parse+cir 116 ms → 29 ms). After these fixes, the remaining parse cost is the token-object traffic (nextToken/peek, virtual kind calls, two-level interning: the global `istring_table` plus the per-Program `strpool`) and the per-node `cir_node`/c2mir bookkeeping (`CirBuilder::make` 22.1M aw / 166.5M sq). Those are the structural items already listed in the tinycc recon §2.1/§2.2.
