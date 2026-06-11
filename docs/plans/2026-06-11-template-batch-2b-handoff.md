# Item 2b handoff — `a + "literal"` free-operator instantiation (analysis done, code not started)

State as of `feature/template-instantiation-claude` @ `398cb82`. 2a (empty-pack
elision) and the iostream:80 warning fix are LANDED and gated on this branch.
2c/2d not started. Reducer: `tmp/strplus_lit.mad` (REGENERATE if gone:
`std::string a = "foo"; std::string b = a + "bar"; std::string c = "pre" + b;`
with `--std=c++17 --no-embedded-headers` — the embedded path MASKS both bugs).

## Verified facts (don't re-derive)

1. **libstdc++ exports** (weak, from the extern-template instantiation;
   `nm -D libstdc++.so.6 | grep _ZStplIcSt11`):
   - `(const char*, const string&)` `…PKS5_RKS8_` — **EXPORTED**
   - `(const string&, const string&)` `…RKS8_SA_` — exported
   - `(char, const string&)` `…S5_RKS8_` — exported
   - `(const string&, const char*)` `…RKS8_PKS5_` — **NOT exported** (count 0)
2. **`"pre" + b` fails** ("invalid operand types of +") because
   `class_operator_call` requires an LHS class and returns NULL otherwise
   (cir_builder.cpp `as_class_instance(top->left…)` … `if (!lcls) return
   NULL;` around :5897–:5911) — the W2 binder
   (`std_free_operator_instantiation`, :5315) is never consulted. The binder
   itself is lhs-class-centric (`collect_self_and_base_spellings(lcls…)` +
   `deduce_lhs`).
3. **`a + "bar"` fails** (integer-for-pointer warning + check error) because
   the matching shape is NOT exported — binding it mangled-direct yields a
   dead import; the BODY must instantiate. But operator templates are never
   retained for body instantiation: `capture_free_operator_overload` returns
   EARLY (parser.cpp:23084) BEFORE the body-retention block that feeds
   `fn_template_map` (parser.cpp:23099–23116, the 2a machinery's source).

## Plan

**2b-i — literal-lhs, exported shape (smaller).** In `class_operator_call`
(or a sibling entry), when `!lcls && as_class_instance(top->right->datadef())`,
try the free-operator path with roles adapted: the binder's Pass-2 (by-value
return) deduction needs to accept param[0] matching the non-class lhs spelling
exactly (e.g. `const _CharT*` vs `const char*` via binding) while param[1]
deduces against the RHS class. Result binds the EXPORTED `…PKS5_RKS8_`
mangled-direct through the existing `class_operator_external_call` (note: its
`lcls` argument is used for receiver shaping — verify what it needs when the
class operand is on the RIGHT; the by-value sret/__retbuf return shape from
the 2026-06-10 `a+b` landing applies).

**2b-ii — string+literal, non-exported shape (bigger).**
1. Retain body-bearing free-OPERATOR templates in `fn_template_map` keyed
   `"std::operator+"`: in `capture_namespace_fn_template` (parser.cpp:23078),
   don't return at :23084 before retention — when the operator capture
   succeeds AND the decl has a body + typeparams, also push the FnTemplateDef
   (opname needed from `capture_free_operator_overload` — extend it to output
   the opname).
2. Instantiate at PARSE time (token injection is parse-time only — CIR is too
   late). Hook: where the parser types `a + "bar"`
   (`Program::resolve_object_operator_type` / `free_binary_operator_return_class`)
   — when operands involve a class and a retained operator template deduces, run
   the 2a injection (`try_instantiate_namespace_fn_template` — needs a synthetic
   TokenCallFunc carrying the two operands as parameters, or a small operator
   variant of the entry point).
3. The instantiated definition registers in `namespace_fn_overload_sets`
   under its own symbol; the CIR operator path must then prefer it (the
   `select_operator_overload`/W2 arbitration in `class_operator_call`) over
   the dead mangled-direct candidate. The 2a fn-ptr signature scoring in
   `score_arg_to_param` already discriminates if ranking is reached.

**Gates per landing:** capped reducer runs, fulltest, FULL torture
failset-diff vs `tmp/failset_lsq.txt` (55 lines), SMAUG soak (operator paths
are SMAUG-hot). teststod{,_realhdr} must stay green (2a regression canary).

## 2c / 2d (unchanged from the 2026-06-11 next-work handoff)

- 2c: decl-path silent ctor-drop → loud error (cir_builder.cpp ~:1030/:1062/
  :1164 `if (cc) m_pending_stmts.push_back(cc);` guards) — expect latent
  surfacing; full gates.
- 2d: `cout << s` const-string&-param (`tmp/rK.mad`) — ATTRIBUTE FIRST
  (gcc + clang + stock `/workspace/mir/c2m` 3-way) before touching anything.

## Leftover noted during the iostream fix

The nested-type declaration fold requires `DataDefCLASS` — plain un-promoted
`struct Outer { struct Inner … }` still doesn't fold (std:: headers are all
classes; revisit with the struct-promotion machinery if real code hits it).
