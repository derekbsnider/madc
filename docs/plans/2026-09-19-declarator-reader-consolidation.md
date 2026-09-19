# Declarator-reader consolidation — the ONE declarator owner

Status: **plan written 2026-09-19 from a `/dupaudit` recon; not started.**
OWNER DECISION 2026-09-19: this is the next work on the C++11 arc (lane
1412/1950 = 72.4%; 🎯 master-release TRIGGER at 75% = 1463, +51). The handoff
ordered: plan first, parser.cpp untouched until the plan is read.

Governing law for this arc (owner, 2026-09-19, "set this in immutable
stone"): **Rule #4.** The recon found NINE existing declarator owners in the
tree. The consolidation COMPOSES them; a new named function is justified only
for the one piece the tree provably lacks, and that null search is cited
below. Every task carries `Searched:` naming the concept and the owner it
adopts. A task that introduces a new helper without a null-search citation is
a plan defect.

Scope of the recon (stated before it ran): `src/parser.cpp` declarator
positions — every site that reads a declarator from the token stream and
builds a derived type from it — plus the existing owners those sites should
have adopted. Every site below was READ, not grepped.

---

## 0. The `/dupaudit` report — three families, ranked by damage

Known-family re-check first: the KG held `c-declarator-partial-grammars`
(status `consolidated`, sites=4). Its marker greps CALLERS of the four owners
it names, so it counts adoption, not readers — it structurally cannot see a
new reader. Annotated `superseded_by = declarator_readers`. Not a regrowth;
a bad marker (the skill's own warning, step 7).

### Family 1 — `declarator_readers` (DIVERGENT, 14 readers + 1 consume-only)

**Rule:** one C/C++ declarator grammar — [dcl.decl] named, [dcl.name]
abstract: ptr-operators (`*` cv, `&`, `&&`, `C::[D::]*` cv), then
`( declarator )` or a declarator-id, then `[dim]` / `(params)` suffixes,
applied inside-out. Every position that reads one from tokens reads it
through ONE recursive owner.

**Sites** (`src/parser.cpp`, ~line; ★ = divergence PROBED against g++):

| # | site | mode | what it lacks / how it differs |
|---|------|------|--------------------------------|
| A | parseFunction parameter arm ~68830–69560 (`grabnt:`, `finish_param_declarator:`, `paramdecl:`) | Parameter | richest; FOUR inline `[dim]` loops; ★ inline `C::*` copy reads ONE `::` and resolves via `struct_map` only (`int N::C::*pm` refused; g++ `r:42`) |
| B | parseDeclaration variable arm ~72170–72470 (`fnptr_decl_arm_head:`) | Declaration | nesting via stash + `goto` re-entry, spiral via a second stash; inline `[dim]` loops ×2; ★ owner lookahead blind to a template-id owner (`int (First<int>::*pf)()` refused; g++ `pf:3`) |
| C | TokenTYPEDEF::parse ~53330–53530 | Named | ★ no `(*N)[K]` ("Expecting '(' for parameter list"; g++ `a3:7 sz:40`); ★ no `(&N)[K]` ("Expecting '*' in function pointer typedef"; g++ `c1:x sz:2`); inline `[dim]` loop in Form 2 |
| D | typedef comma-list tail lambda ~53143 | Named | stars+cv inline; arrays via the FLATTENING owner (Family 2) |
| E | TokenSTRUCT tag-typedef list ~45645 | Named | stars inline; `(*` → `parse_fnptr_member_tail` (adopts); arrays via Family 2 |
| F | TokenSTRUCT body typedef alias ~46895 | Named | stars inline; arrays via Family 2 |
| G | TokenSTRUCT member arm ~46519–46640 | Named | adopts stars/mp owners; inline member `[dim]` loop, dims MULTIPLIED into `member_count` |
| H | TokenCLASS member arm ~50940–51020 (+dims ~51274) | Named | same as G plus `ret_is_ref` |
| I | K&R parameter declaration ~67910 | Named | `(*name)(params)` inline; stars+cv inline |
| J | using-alias arm ~44380–44450 | Abstract | ★ NO array suffix at all (`using A = int[3]` → "Expecting ';' after using alias"; g++ `a:3 b:5 sz:12`); ★ no `(*const)[K]` / `(&)[K]` ("Expecting ')' in function pointer alias"; g++ `t2:2 u1:9`) |
| K | template-argument type-id reader ~29380–29430 | Abstract | stars/refs inline; `(*)(params)` and bare `(params)` only; ★ no `(C::*)(A)` (`B<int (Foo::*)(int)>` refused; g++ `n:5 sz:16`); no arrays |
| L | cast type-id arm ~41630–41690 | Abstract | inline single-dim `[N]` that accepts only an INTEGER LITERAL; `(*)(params)`/`(*)[N]` |
| M | sizeof/alignof type-id operand ~15140–15210 | Abstract | stars inline; adopts mp owners; skips the mp-fn parameter list with a HAND-ROLLED `mdepth` counter (delimiter-tracking violation the gate's marker misses) |
| N | `parseFnPtrParams` per-parameter declarator ~53830–53960 | Parameter | its OWN reader: stars/cv/ref, nested `(*name)(args)`, abstract fn, named fn type; no arrays, no `C::*` parameter types |
| O | `consume_template_parameter_declarator` ~55828 | consume-only | reads the SHAPE (ptr-ops, nested `(`, suffixes) and reports name + pack; builds no type — see Ruling 7.1 |

**Divergence when hit:** parse REFUSALS of valid C++11 (every ★ above); the
g++.dg cluster at HEAD is 20 compile-fails + 1 timeout (`template/ptrmem19.C`,
the typedef arm looping on `typedef T2 (T1::*m)(T3)` inside a class template).

**Tie-breaker — would a change to the rule require editing more than one
site?** YES, fourteen times over: adding `(&name)[N]` today means editing A,
B, C, J, K, L and N separately; the array-suffix rule alone lives in eleven
loops. Duplication, not separation of concerns.

**Consolidation sketch:** §2. **Gate:** §3.

### Family 2 — `array_suffix_readers` (DIVERGENT, 2 owners + 9 inline loops)

**Rule:** a declarator array suffix `[N][M]...` nests one `DataDefCArray` per
dimension, outermost first (C11 6.7.6.2); a runtime dim becomes `count_expr`.

**Sites:** `parse_ptr_array_suffix` ~53535 (owner — NESTS, then wraps PTR);
`parse_typedef_array_suffix` ~19320 (★ **FLATTENS**: `alias_count *= n`);
inline loops in A ×4 (~69080, ~69155, ~69260, ~69370), B ×2 (~72270, ~72380),
C ×1 (~53400), G ×1 (~46631), H ×1 (~51274), L ×1 (~41640).

**Divergence when hit — a WRONG SHAPE, not a refusal at the typedef:**
`typedef int M[2][3]; M m; m[1][2]` → "subscripted value is neither array nor
pointer" (g++ `sz:24 e:12 d:5`). `peel_carray_dimensions` (~71598) already
walks a NESTED chain, so the flat typedef peels as ONE dim of 6 — the
consumer is right and the producer is wrong. Under the project's own rules a
wrong answer outranks a refusal; T2 fixes this FIRST.

**Tie-breaker:** yes — `[static 5]`/`[*]` param qualifiers had to be added to
four A loops separately (`skip_param_array_qualifiers` exists for exactly
that and the other seven loops never call it).

### Family 3 — `member_pointer_owner_chain` (DIVERGENT, owner + 1 copy; owner INCOMPLETE)

**Rule:** the `C::[D::]*` owner chain of a pointer-to-member declarator is read
by `parse_member_pointer_owner` after `member_pointer_declarator_ahead` says
yes — in every declarator position, template-id owners included.

**Sites:** owner ~53630 (adopted by B, G, H, M); ★ inline copy in A ~69290–
69320 (one `::`, `struct_map` only — probe p9). ★ The owner's LOOKAHEAD cannot
see a template-id owner (`First<int>::*`, probe p7; `conv1.C`) — "owner exists
≠ owner complete": complete it there (a balanced `<…>` group is a NAME
question per `delimiter-tracking.md`; the lookahead walks `tokens[]` by index
→ `delim_scan_step(seq, i, d)` with `DelimDepth d(this)`).

**Tie-breaker:** yes — p9 is exactly the edit that reached the owner and not
the copy.

Dropped from the report (cap is three): the flattened struct-member dims
(G/H `member_count`) — same rule as Family 2, folds into T8; the K&R arm —
folds into T9.

---

## 1. Rule #4 inventory — the owners the consolidation is COMPOSED from

| concept | owner (`src/parser.cpp`) | note |
|---|---|---|
| stars + interstitial/east cv | `consume_declarator_stars(dd&, out_const_after_star, …)` :1627 | returns star count |
| cv skip | `skip_cv_qualifier_tokens()` :1681/1689 | |
| `C::[D::]*` lookahead | `member_pointer_declarator_ahead(first) const` :53594 | to COMPLETE (template-id owner) |
| `C::[D::]*` consume + resolve | `parse_member_pointer_owner(first, owner_name&)` :53630 | |
| `(C::*name)(params) cv noexcept` | `parse_member_fnptr_declarator(returns, mname&, first)` :53665 | |
| `[cv] name? ) ( params )` | `parse_fnptr_member_tail(returns, mname&, open)` :53689 | already handles the ABSTRACT `)` |
| parameter-type-list | `parseFnPtrParams(returns)` :53762 | keeps its list loop; its per-PARAMETER declarator (site N) delegates |
| `[N]...` nested, PTR-wrapped | `parse_ptr_array_suffix(elem, ctx, what, capture_runtime)` :53535 | becomes the one dim reader |
| `[N]...` flattened | `parse_typedef_array_suffix` :19320 | DELETED in T2 |
| param bracket qualifiers | `skip_param_array_qualifiers()` :53747 | Parameter mode only |
| runtime dim decision | `bracket_dim_needs_runtime_value(names)` :19307 | |
| balanced group capture | `DelimDepth d(this)` + `delimStepStream(t, d, &stash)` | the stash technique B already uses |
| pack ellipsis | `consume_ellipsis()` :55565 | shape only; expansion is the tsubst spine's (§6) |
| comma re-entry | `comma_continuation_starts_declarator` :53736 | unchanged |
| alias peel | `peel_carray_dimensions` :71598 (static) | consumer; already nested-aware |

**Null search for the one new piece.** Concept: "recursive declarator reader
returning declared type + declarator-id, applying nested `( … )` inside-out".
`grep -n 'parse_declarator\|parseDeclarator\|read_declarator\|DeclaratorMode\|DeclaratorResult\|struct Declarator\|nested_declarator_opens' src/*.cpp include/*.h` → **no hits** (2026-09-19). The recursion exists nowhere; B fakes it
with `nested_decl_stash` + `goto fnptr_decl_arm_head`, A enumerates shapes.
That — and only that — is what gets written.

---

## 2. The ONE owner

```cpp
// include/madc.h (Program members), src/parser.cpp beside parse_ptr_array_suffix
enum class DeclaratorMode {
    Named,        // typedef / member / K&R: a declarator-id is REQUIRED
    Abstract,     // type-id (alias target, template argument, cast, sizeof): FORBIDDEN
    Parameter,    // optional id; C99 6.7.6.3p7 bracket qualifiers; VLA dims may
                  //   name earlier parameters (runtime_names); decay is the CALLER's
    Declaration   // variable/function declaration: a `(` after the declarator-id
                  //   is a FUNCTION declarator — STOP, leave it on the stream
};
struct DeclaratorResult {
    std::string name;  TokenBase *name_tok = NULL;
    bool is_pack = false;                 // `...` before the id (consume_ellipsis)
    int  ptr_depth = 0;                   // stars applied to the BASE (A's param_ptr_depth)
    bool const_after_star = false;        // consume_declarator_stars' out param
    RefType ref = RefType::rtValue;  bool rvalue_ref = false;
    std::vector<carray_dim_t> array_dims; std::vector<TokenBase *> array_dim_exprs;
    bool saw_parens = false;              // a `( declarator )` was read
    bool function_pending = false;        // Declaration mode: `(params)` left unread
};
DataDef *Program::parse_declarator(DataDef *base, DeclaratorMode mode,
                                   DeclaratorResult &out,
                                   const std::set<std::string> *runtime_names = NULL);
```

Algorithm (the textbook inside-out reader, built from §1):

1. **ptr-operators**, applied to `base` as read: `*` cv → `consume_declarator_stars`
   (records `ptr_depth`, `const_after_star`); `C::[D::]*` cv →
   `member_pointer_declarator_ahead` + `parse_member_pointer_owner` →
   `DataDefMemberPtr(owner, base)` (the DATA form; a later `(params)` suffix
   converts it to `DataDefMemberFnPtr`, so the function form is the SAME read
   plus a suffix — `parse_member_fnptr_declarator` becomes the owner's
   `C::*` + `(params)` path); `&` / `&&` → recorded in `out.ref`, applied
   LAST (a reference is always outermost: `T (&)[N]`, `T *&`).
2. **direct-declarator**: if `(` and `nested_declarator_opens()` — the
   predicate O already spells inline at ~55873: `(` followed by `*` `&` `&&`
   `(` cv, or `ident ::` (extend: a template-id owner) — then STASH the
   balanced inner group (`DelimDepth d(this)`, `delimStepStream`), consume
   `)`, parse THIS level's suffixes (step 3) on `base` → `T1`, push the stash
   back in reverse, and `return parse_declarator(T1, mode, out, …)`. This is
   B's technique made recursive; the `goto` and both stashes go away. Else in
   Named/Parameter/Declaration modes read the declarator-id (the SAME token
   rule `typedef_alias_spelling` accepts — a datatype token being redefined is
   a legal id); Abstract mode reads none.
3. **suffixes**, loop: `[` → the ONE dim reader (T2's core of
   `parse_ptr_array_suffix`; Parameter mode calls `skip_param_array_qualifiers`
   and passes `runtime_names`); `(` → in Declaration mode with a declarator-id
   at THIS level: `out.function_pending = true`, return (parseFunction reads
   names + body itself); otherwise `parseFnPtrParams(T)` → `DataDefFPTR(func)`
   with `ptr_syntax = false` (a FUNCTION type), then trailing cv / ref-qual /
   `noexcept` (what `parse_member_fnptr_declarator` reads today).
4. **fold rules** (the ones every arm hand-rolls differently today):
   a `*` applied to a FUNCTION type sets `ptr_syntax = true` on that FPTR
   (madc models `R (*)(A)` as ONE `DataDefFPTR`, not PTR(FPTR)); a further
   `*` is `getPointerType` (B's `fnptr_extra_stars`); `*` applied to a
   `DataDefMemberPtr` whose suffix was `(params)` yields `DataDefMemberFnPtr`;
   `&` applied to a CArray keeps the array complete (A's
   `param_dd->as_pointer_dd()->base_type` dance disappears).

**Caller side-state derives from `DeclaratorResult`, never re-read:** A's
mangling spelling (`param_ptr_depth`, `param_array_dims`, `rtype`,
`param_rvalue_ref`, `param_leading_const`) is computed from `out` exactly as
today at `paramdecl:`; B's `arr_dims` / `vla_size_expr` / `decl_fnptr_stars`
/ `have_decl_id` likewise. That is what keeps the Itanium spellings
byte-identical (the `PKc`-not-`Pc` comment at ~69455 is the contract).

**Thread-safety contract:** parser state is per-`Program`, single-threaded
per parse; no new globals; `DeclaratorResult` is a stack value.

---

## 3. The gate — `scripts/check-one-declarator-reader.sh` (fulltest)

Pattern: the ratchet gate (`check-one-pointer-peel.sh`): counts that may
never RISE above BASELINE, lowered in the same commit as each migration,
with a negative control proving each marker still matches the owner.

Markers (all over `src/parser.cpp`; measured 2026-09-19 at 85f3c91d4):

| marker (the CONCEPT) | grep | today | end state |
|---|---|---:|---:|
| parameter-type-list readers | `parseFnPtrParams([^)]` (calls with an argument; comments say `()`) | 15 | 2 (definition + the owner's suffix call) |
| fn-ptr type construction | `new DataDefFPTR(` | 17 | 10 (owner ×1 + 9 non-declarator sites*) |
| array type construction | `new DataDefCArray(` | 10 | 5 (owner ×1 + 4 non-declarator sites*) |
| data member-ptr construction | `new DataDefMemberPtr(` | 6 | 2 (owner + `&C::field` constant ~31982) |
| fn member-ptr construction | `new DataDefMemberFnPtr(` | 3 | 2 (owner + `&C::method` constant ~31966) |

\* non-declarator (tree-building, not token-consuming) sites, allowlisted BY
NAME in the gate header: `peel_carray_dimensions`-style rebuild ~3245,
`__builtin_va_list` ~3505, ClassTypePattern CArray ~8260, string-literal type
~13892, `&C::m` constants ~31966/31982, ternary call typing ~42332, deduction
decay ~60273, fn-ptr variable from a function ~72627, VLA element ~73831.

Negative control: the owner's own construction lines (`parse_declarator`'s
`new DataDefFPTR(` and `new DataDefCArray(`) must match, or the gate fails.
A count BELOW baseline without a BASELINE edit also fails (the pointer-peel
precedent: "if the owner was renamed, update the gate in the same commit").
Wire after `check-one-delim-tracker.sh` in `src/Makefile` (~1098). KG
`DupFamily` nodes `declarator_readers`, `array_suffix_readers`,
`member_pointer_owner_chain` go `open → consolidated → gated`.

---

## 4. Sequencing — lowest blast radius first, the two hot arms LAST

Every task: own commit with the four trailers; reducers land WITH their fix
(`tests/testdecl*.mad` + `.flags` = `--std=c++11` + `.expect` from g++ AND
clang++); targeted validation = the new tests + `bash scripts/run_tests.sh`
locally with the failure LIST diffed against `tmp/cxctor/f7.txt` (never a
count); gate BASELINE lowered in the same commit. The lane (6 min) runs after
T6 and at the end. The seam battery runs ONCE, at the arc's seam.

The probes in `tmp/declprobe/p1..p9` ARE the reducers; their g++ outputs are
in §0. Rename into `tests/` as each task lands them.

**T0 — the gate at today's baseline.** `scripts/check-one-declarator-reader.sh`
with the §3 table as BASELINE, classification comment, negative control
(temporarily against `parse_fnptr_member_tail`'s construction line until the
owner exists), Makefile wiring. Freezes the family so the arc cannot grow it.
Searched: ratchet-gate precedent → `check-one-pointer-peel.sh`.

**T1 — complete the lookahead (Family 3, owner side).**
`member_pointer_declarator_ahead` learns a template-id owner segment by
walking `tokens[]` with `delim_scan_step` (DelimDepth constructed WITH the
Program — the `<` question is a name question). Reducer p7
(`int (First<int>::*pf)()` → `pf:3`). Acceptance: `template/conv1.C`.
Searched: balanced-delimiter tracker → `DelimDepth` index form; angle-open →
`class_member_lt_reading`/`unqualified_name_lt_reading`.

**T2 — ONE array-dimension reader (Family 2).** Extract the dim loop of
`parse_ptr_array_suffix` into `parse_array_dimensions(dims&, exprs&, mode,
ctx, what, runtime_names)` (null search: `grep -n 'parse_array_dim\|read_array_dim\|array_dimensions(' src/ include/` → none);
`parse_ptr_array_suffix` becomes dims + nest + `getPointerType`;
`parse_typedef_array_suffix` becomes dims + nest (NO flatten) and keeps its
forest write-through; then DELETE `parse_typedef_array_suffix` by pointing
its 4 callers (C, D, E, F) at the nesting builder. Trace first:
`peel_carray_dimensions` ~71598 (nested-aware ✓), `DataDefCArray::count`
consumers of a typedef'd array (sizeof = count × element size — unchanged
under nesting), forest v25 `forest_arena_record_unary`. Reducer p1
(`sz:24 e:12 d:5`). This is the wrong-answer fix; it ships before any
refusal fix.

**T3 — the owner, first adopter = using-alias arm (J, Abstract).** Write
`parse_declarator` per §2 from the §1 owners. J's body (~44394–44450)
becomes: resolve target → `parse_declarator(alias_dd, Abstract, out)` →
`;`. Reducers p4 (`a:3 b:5 sz:12`), p5 (`t2:2 u1:9`). Acceptance:
`initlist-array17/20/6/22`, `ref-bind1`. Existing negative controls:
`tests/testaliasmemberfnptr.mad`, every `using X = …(*)(…)` in libc++/libstdc++
headers the suite already parses.

**T4 — template-argument type-id (K, Abstract).** ~29380–29430 → the owner.
The resulting DataDef must produce the SAME `canonical_arg_key_fragment` /
spelling as today for `(*)(params)` and bare `(params)` — assert with the
existing template-key tests (`testptrbuiltinspec.mad` and kin) before/after.
Reducer p6 (`n:5 sz:16`). Acceptance: `canon-type-6/7`, `mem_func_ptr`,
`sfinae22`; probe `defaulted50` here (its "Unknown type 'Foo'" may be
resolution, not grammar — report which).

**T5 — cast (L) + sizeof/alignof (M) type-ids (Abstract).** Retires the
literal-only `[N]` in L and the private star / `C::*` / `(C::*)(…)` / `(*)(…)`
loop in M (with its two hand-rolled depth counters). The owner's Abstract
mode stops before a `(` that begins no parameter list (`(T(x))`,
`sizeof(T(5))` — the enclosing expression's paren), so the cast arm's
push-back fallback and the sizeof arm's span-check rewind keep working. The
compound-literal bookkeeping (`(int [3]){…}`: element type + count) derives
from the owner's result when the type-id was a bare array. Reducer
`tests/testdeclsizeofdecl.mad`.
⚠️ The delimiter-gate marker widening this task originally carried is
WITHDRAWN: measuring it (`int [a-z_]*depth[a-z_]* = [01]`) found ~60 hand-rolled
counters in parser.cpp alone — the `angle_bracket_depth_tracking` family has
regrown under names its spelling-keyed marker cannot see. That is a
classification + migration ARC of its own (recorded on the KG family, §6),
not a side task here.

**T6 — typedef arm (C) + list tail (D) + struct-tag list (E) + struct-body
typedef (F) (Named).** C's `(` arm and Form 1/Form 2 posture collapse into
one `parse_declarator(base, Named, out)`; `ptr_syntax` follows §2 rule 4
(`(*N)(p)` true, `N(p)` and `(N)(p)` false — winnt.h:5870/5871 shapes are the
regression tests); GNU attributes stay where they are (before/after the
declarator, caller-side). Reducers p2 (`a3:7 sz:40`), p3 (`c1:x sz:2`).
Acceptance: `lambda-defarg5`, `sfinae43`, `collapse-bug`, `ptrmem19` (the
timeout — verify it now terminates in < 2 s). **Run the lane here.**

**T7 — `parseFnPtrParams` per-parameter declarator (N, Parameter).** The
list loop stays; each parameter's stars/cv/ref/nested-paren block (~53850–
53960) becomes `parse_declarator(param_dd, Parameter, out)`; spellings
(`param_cpp_spellings`) derive from `out` exactly as the block does today.
NOT in scope: pack expansion `T...` (§6). Reducer: `typedef void F(int (*)[2], char (&)[3]);` + a call through it.

**T8 — struct (G) and class (H) member arms (Named).** Dims via the T2
reader; `member_count` multiplication is replaced by the nested CArray the
member already stores for subscripting — trace `addMember`'s count consumer
and `member_dims` first. Bit-fields stay caller-side (`parse_bitfield_width`).
Reducer: `struct S { int (*fp)(int); int m[2][3]; char (&r)[2]; }` sizes.

**T9 — K&R parameter declaration (I).** Trivial adoption; reducer from
c-testsuite shapes already green (00089/00124/00162/00209 are the negative
controls — run `scripts/run_c_testsuite.sh` subset or the equivalent lane).

**T10 — parseFunction parameter arm (A, Parameter).** The `grabnt:` loop,
the four dim loops, the `(` shape ladder and the inline `C::*` copy become
ONE call; `paramdecl:` reads `out` for `rtype`, `param_ptr_depth`,
`param_array_dims/_exprs`, `param_rvalue_ref` and builds `param_spelling`
UNCHANGED. Decay stays at `finish_param_declarator:`. Reducer p9 (`r:42`).
Negative controls: the mangling tests (every `_Z…` `.expect`), `testfnptr*`,
`testvla*`, `--emit=c11` of a K&R + prototype pair. Highest mangling risk —
compare `bin/madc --emit=c11` output of the whole `tests/` corpus before and
after (byte-identical except the new tests).

**T11 — parseDeclaration variable arm (B, Declaration).** Both stashes and
`fnptr_decl_arm_head:` go; `function_pending` routes to parseFunction with
the built return type (the spiral `type (*name(fn-params))(ret)` is the
owner's recursion + the stop rule). Trace first: `is_fnptr_base` /
`decl_fnptr_stars` (emitter legacy path, -1 sentinel), `saw_pointer_decl`,
`alias_dim_count`, `have_decl_id`, `ret_is_ref` (return-reference vs
declarator-reference split at ~72300). Hottest path in the parser → last,
with `bash scripts/run_tests.sh` AND `--exe` locally before the commit.

**T12 — `consume_template_parameter_declarator` (O) — Ruling 7.1 applied.**
Replace its inline `tokens[1]->id() == tkMul || …` ladder with the owner's
`nested_declarator_opens()` predicate and its ptr-operator test with the
owner's; it keeps consuming rather than building. Gate: the ladder spelling
may not reappear.

**T13 — close.** BASELINE at end state; KG families → `gated`; AGENTS.md Rule
#4 "standing instances" gains "a declarator is read by
`Program::parse_declarator`"; `delimiter-tracking.md` unchanged; ROADMAP 2.12
updated with the measured lane; HANDOFF rewritten.

---

## 5. Measured acceptance

- The **15** cluster tests: `cpp0x/collapse-bug`, `cpp0x/initlist-array17`,
  `-20`, `-22`, `-6`, `cpp0x/lambda/lambda-defarg5`, `cpp0x/ref-bind1`,
  `cpp0x/sfinae43`, `template/canon-type-6`, `-7`, `template/conv1`,
  `template/mem_func_ptr`, `template/sfinae22`, `template/ptrmem19` (timeout
  → pass), and `cpp0x/defaulted50` if T4 shows it is grammar.
- Lane: 1412 → **≥ 1426/1950**, 0 outside baseline; baseline ratcheted.
- Local suite: failure LIST byte-identical to `tmp/cxctor/f7.txt` plus the new
  `testdecl*` passing; `--emit=c11` corpus byte-identical (T10/T11).
- Gate: BASELINE table at end state; negative control green.
- `scripts/check-rule-trailers.sh`: 0 missing.

---

## 6. Recorded, NOT taken (with their owners — Rule #4 applies to them too)

- **Pack expansion in a parameter-type-list** (`variadic15`, `variadic22`,
  `variadic-nested2`, `variadic29`): owned by the tsubst spine — "a pack
  expansion in a parameter-declaration is N DECLARATIONS" (~parser.cpp:11082)
  and `clone_template_tokens_with_type_subst` (~5840). A class-template BODY
  typedef reaches `parseFnPtrParams` with the pack unexpanded: a substitution-
  lane site that never adopted that owner. Own session (parse-once KIND work),
  not a declarator change.
- **[stmt.ambig] declaration-vs-expression** (`ref-bind6` `A<const int*>(a.m);`,
  `trailing7` `int(f()->i);`): the variable arm commits to a declaration on a
  leading type name. Once the owner can report "not a declarator" in
  Declaration mode this becomes a tentative-parse fallback — a separate
  ruling, +2 tests.
- **The delimiter family has REGROWN under other spellings.** The gate
  (`check-one-delim-tracker.sh`) reports 0 because its marker is keyed on
  `angle|paren|square|brace` in the counter's NAME; `int [a-z_]*depth[a-z_]* = [01]`
  finds ~60 locals in parser.cpp (`depth`, `adepth`, `pdepth`, `bdepth`, `mdepth`,
  `nd`, `spiral_depth`, …) — some legitimate recursion depths, most balance
  loops (`consume_balanced_parenthesized_suffix` ~68120,
  `paren_group_is_function_def` ~71656, parseFunction's `noexcept(...)`
  capture ~69834, `template_id_suffix_end` ~5936). Recorded on the KG family
  (`regrowth_2026_09_19`). Its own arc: classify every hit, migrate the
  balance loops to `DelimDepth`, THEN widen the marker. T5 retires the two
  sizeof ones by adoption; nothing else here touches them.
- **Struct/class member dims flattened** into `member_count` — T8.
- Literal-operator mangling (`_Z12operator_qu_qu_wm` vs Itanium `li`) — the
  mangling feature's owner, unchanged from the 2026-09-18 handoff.

- **The emitter twin of this family (found by T3).** `CirBuilder::typedef_decl`
  unwrapped ONE pointer level and never peeled the pointee's dims, so
  `typedef int **PP` and `typedef arr10 *PA` both emitted `typedef int *`
  (SILENT; c2mir's checker refused `**pp` / `(*pa)[2]`). `var_decl` had the
  rule right. Fixed in its own commit through ONE owner (`peel_pointer_declarator`
  + `CirBuilder::append_pointer_declarator`) adopted by both; reducer
  `tests/testtypedefptrarray.mad`. The parameter (~8933) and member (~10474)
  declarator emitters still compose their own pointer piece — KG family
  `cir_declarator_pieces`; their adoption and the fulltest gate land at T13.
- **T11 probe (found by the emitter fix's reducer).** A VARIABLE declared through
  a typedef'd ARRAY OF POINTERS with an initializer — `typedef const char
  *names[3]; names n = {"a","bb","ccc"};` — emits `names *n[3] = {...}`: the
  variable arm flattens the typedef's dims into `v->dims` and leaves the
  ELEMENT pointer as `v->type`, so `explicit_star_count` re-adds the alias's
  star and the dims survive the `!is_ptr` skip gate. Three "incompatible types"
  warnings, `n[2][0]` reads garbage (g++/clang++: `n:a ccc n2:ccc t:3`).
  Chain: parseDeclaration var arm (flatten) -> Variable{type,dims,typedef_name}
  -> var_decl's compensation. The fix is T11's: the owner returns the alias's
  DataDef plus the variable's OWN pieces, and the flatten + `explicit_star_count`
  compensation go. Reducer parked: `tmp/declprobe/pending-tests/testtypedefarrayofptrinit.*`.

## 7. Rulings

7.1 **`consume_template_parameter_declarator` stays a consumer.** It answers
"where does this declarator END and what is its NAME" for a dependent type the
tree cannot yet build; the owner answers "what TYPE does it declare". Same
GRAMMAR, different output — so the SHAPE predicates (`nested_declarator_opens`,
the ptr-operator test) are shared and the type build is not. Forcing it to
build a type would invent a dependent-type builder beside the tsubst spine —
the new-lane trap. Tie-breaker: a change to the grammar's shape edits ONE
predicate (shared); a change to type construction edits ONE owner (not it).

7.2 **`parse_typedef_array_suffix` is deleted, not kept as a mode.** Two
owners for one rule is the family's live divergence; the flatten has no
legitimate consumer (`peel_carray_dimensions` proves it).

7.3 **`ptr_syntax` semantics are preserved exactly** (§2 rule 4); the emitter
is not touched by this arc. If a fold rule cannot reproduce today's
`ptr_syntax` for a green shape, the shape is a regression test, not a
tolerance.

7.4 **Order is blast-radius, not yield.** The owner's first adopters are
the type-id arms (J, K, L, M) because they cannot change a mangled symbol;
the parameter and variable arms are last because they can.
