# Expression SFINAE — a substitution failure in a function template's declaration declines the candidate

Status: **plan written 2026-09-19 from the 1441 first-error histogram; not started.**
OWNER DIRECTION 2026-09-19: "return focus on breaking the 75% barrier" (lane
1441/1950 = 73.9%; 🎯 master-release TRIGGER at 75% = 1463, **+22**). Owner
law for parser work: plan first, `src/parser.cpp` untouched until the plan is
read ([[feedback_no_impromptu_parser_changes]]).

Governing law (owner, 2026-09-19, "immutable stone"): **Rule #4.** Every
owner named below was READ, not grepped. A task adopts an existing owner or
cites the null search that justifies a new named function.

---

## 0. Measurement — why this construct

The final declarator-arc lane (T11 binary) has 509 failing tests. Their first
errors, histogrammed by CONSTRUCT (test-name family, then the source line at
the error), not by message:

    sfinae:49  pr:34  initlist:42  variadic:22  decltype:19  noexcept:17
    constexpr:13  range-for:10 ...

**85 of the 509 failing tests carry a dependent `decltype(...)` in a template
declaration head** (return type, trailing return, or a default template
argument) — the single biggest construct. 24 of the sfinae tests fail with
their own `static_assert` FIRING: madc binds the overload SFINAE should have
removed. Those are **silent wrong answers** in ordinary code (exit 0, wrong
overload), which by owner law outrank everything else in the queue
([[feedback_found_it_you_fix_it]]).

Probes (all in `tmp/declprobe/probe/`, g++ oracle beside each; clang++ agrees):

    sfinae.mad   f<A>:4 f<B>:1     ✓  type SFINAE (typename T::type) WORKS
                 g<int>:4 g<B>:4   ✗  decltype(T() + 1), B has no operator+ → g++ 1
                 h<int>:8 h<ND>:8  ✗  decltype(new T), ND() = delete       → g++ 1
    k1.mad       decltype(T::nonexistent) k1(int); sizeof(k1<B>(0))
                 → HARD ERROR "Expecting identifier" (g++: 1, picks the ... overload)
    k7.mad       template<class T, class = decltype(T::x)> char d1(int)
                 d1<C>:1 (C has no x)  ✗ g++ 2      [dtprobe] operand -> 'void*'
                 template<class T, class = decltype(T() + 1)> char d2(int)
                 d2<C>:1               ✗ g++ 2
                 auto k6(int) -> decltype(T() + 1);  k6<C>:4  ✗ g++ 1
    sfinae2.mad  k4 = decltype(T()) default: int& / abstract → 1  ✗ g++ 2
                 k5 = decltype(T{}) default: int[] / void()  → 1  ✗ g++ 2

The k1 backtrace (gdb `catch throw`) names the layer:

    #3 parseExpr_dataTypeArm  ← THROW
    #5 resolve_declared_type_token
    #6 resolve_type_token_range
    #7 resolve_fn_template_return_by_key      ← the SubstDecl lane
    #8 resolve_namespace_fn_template_call_return_type
    #9 parseCallFunc                            ← the CALL, not registration

So registration is inert (correct); the substitution lane exists and runs at
the call; the defects are (a) a throw during one candidate's substitution is
not a decline, and (b) three operand evaluations answer leniently under a
CONCRETE binding, so the substitution "succeeds" when [temp.deduct]/8 says it
must fail.

## 1. What exists — the owners this plan adopts (Rule #4)

| Concept | Owner (READ) | Where |
|---|---|---|
| Fn-template registration retains the DEPENDENT return tokens, per-param default runs, constraint runs, param type runs | `FuncDef::member_template_return_tokens`, `member_template_param_defaults`, `member_template_param_constraints` | madc.h:410-426; parser.cpp ~65596-65665 |
| Call with explicit args / unevaluated deduced call forms the return by substitution ("clang SubstDecl model") | `parseCallFunc` → `resolve_namespace_fn_template_call_return_type` → `resolve_fn_template_return_by_key` | parser.cpp 30196-30222, 64970, 65031 |
| Candidate DECLINE on a NULL return: "takes the FIRST candidate whose return RESOLVES (a substitution failure `continue`s to the next)"; ellipsis catch-all ordered last ([over.ics.ellipsis]) | the candidate loop in `resolve_fn_template_return_by_key` | parser.cpp ~65100-65140 |
| Default template-argument run → concrete type in an isolated stream; `require_full_parse` for SFINAE constraint evaluation | `resolve_template_param_default_type` | madc.h:6425 |
| [temp.deduct]/8 precedent: a NON-DEPENDENT substitution failure returns NULL ("exactly as clang's SubstType returns a null QualType"); `diagnostics.resize(saved)` + `last_error = saved` restore | alias-template use lane | parser.cpp 11976-12030 |
| Speculative evaluation is MUTED; "a speculative/SFINAE caller catches this like any other decline" | `DiagnosticRenderMute` (RAII) | parser.cpp 227, 44753, 70666, 16280 |
| Unevaluated operand parse (RAII depth, class-pattern poison, `MADC_DT_PROBE`) | the `decltype(` type-specifier arm | parser.cpp 13150-13200 |
| Constructibility of a class from N args: "does ONE non-deleted ctor…" | the `__is_constructible` owner | parser.cpp 15948, 16157 |
| Destructibility (references destructible; deleted dtor not) | the `__is_destructible` owner | parser.cpp 15689 |
| Deleted / pure-virtual facts | `FuncDef::is_deleted`, `FuncDef::pure_virtual` | madc.h |
| Access diagnostic | "'X' is a private member of 'X'" (4 lane tests already reach it) | parser.cpp |

Null search cited here for the ONLY new named thing this plan may add: a
predicate "is value-/list-initialization of type T well-formed" — searched
`is_constructible\|default_constructible\|value_init\|can_construct` in
parser.cpp: the `__is_constructible` owner exists for CLASS types with N args
but nothing answers it for non-class shapes (reference, function type, array
of unknown bound). T4 EXTENDS that owner (one predicate) rather than adding a
sibling.

## 2. The defects — each a layer, each with its trace

**D1 — a throw inside a candidate's substitution is not a decline.**
`resolve_fn_template_return_by_key` `continue`s on NULL but a diagnostic
thrown while resolving the substituted tokens (k1: `T::nonexistent` with T=B)
escapes as a hard error. [temp.deduct]/8: an invalid type or expression in
the immediate context is a deduction failure — the candidate is not viable.
Layer: the candidate loop of that resolver (deepest: it owns viability).
Fix: per-candidate `try` under `DiagnosticRenderMute`, restoring
`diagnostics`/`last_error` as the alias lane does; a throw == NULL ==
`continue`. The default-run resolution (`resolve_template_param_default_type`
with `require_full_parse`) must sit INSIDE that guard when reached from here.
Negative control: a throw from the LAST viable candidate must still surface
(the ellipsis catch-all exists in every SFINAE pair; with no catch-all the
call is an error, as g++ says "no matching function").

**D2 — an ABSENT static member of a CONCRETE class answers `void*`.**
`decltype(T::x)` with T=C (no `x`) resolves (dtprobe `operand -> 'void*'`), so
the default "succeeds". The lenient stand-in belongs to a DEPENDENT shell
(unbound param, opaque placeholder — `datadef_has_unresolved_dependent_surface`);
under a complete concrete class it is a lookup failure. Layer: the qualified
static-member lookup arm reached from `parseExpr_dataTypeArm` — TRACE FIRST
(gdb breakpoint on the dtprobe print, walk `expr`'s origin) before editing;
the hypothesis is a dependent-shell stand-in leaking to concrete classes.

**D3 — a built-in operator on a CLASS operand with no viable overload yields
the built-in result.** `C() + 1` types as `int` (dtprobe `operand -> 'int'`).
[over.match.oper]: with no viable member, non-member (incl. ADL) or built-in
candidate (a built-in candidate needs a conversion function on the class
operand), the expression is ill-formed. Layer: the binary/unary operator
resolution site (`rewritten_operator_candidate` and the arm that falls to the
built-in result) — the parser must decide; today c2mir rejects the emitted C
later, so evaluated code gets a WORSE diagnostic and unevaluated code gets a
wrong type. WIDEST blast radius in this plan: the dialect carriers (`var` /
`value` / `array`) have real operators — every dialect arithmetic test is a
neighbour; conversion functions (`operator int()`) must keep enabling the
built-in candidate. Runs LAST.

**D4 — value-/list-initialization and `new` never consult constructibility.**
`T()`, `T{}`, `new T` are accepted for: a reference type, a function type, an
abstract class, an array of unknown bound, a class with a deleted default
ctor, an array of such, and a temporary of a class with a deleted dtor.
[dcl.init]/[expr.new]/[class.abstract]/[class.dtor]. Layer: the functional-
cast / braced-functional-cast / new-expression arms, calling ONE predicate
that EXTENDS the `__is_constructible` owner (non-class shapes) plus
`__is_destructible` for the temporary. Fixing the construction site makes
evaluated code diagnose too (g++ parity), not only SFINAE.

**D5 — `static_cast<To>(From)` validity and `sizeof` of void/function.**
sfinae9's five cases (to an abstract class, const-dropping pointer conversion,
downcast from a pointer to const base, member-pointer conversion, void→class)
and sfinae38 (`sizeof(void)`, `sizeof(void())`). Layer: the static_cast arm's
conversion predicate and the sizeof arm. Measure first — some may already
throw; the probe decides the rung list.

**D6 — access control inside an unevaluated operand.** sfinae63 (private
member function called in `decltype`) and sfinae37 (private member TYPE via
`typename T::type`). The member-FUNCTION diagnostic exists; the member-TYPE
access check likely does not. Layer: member-type lookup (the `typename`/`::`
type arm). Measure after D1 — a thrown access error is a decline for free.

D6b (measure only, after D1–D5): `decltype` as a partial-specialization
argument (`check<decltype(declval<T&>().f())>::type`, sfinae3/50/63) rides the
class-pattern probe lane (`eval_decltype_probe_tokens`) and should inherit
these fixes; if it does not, that is the next plan, not a task here.

## 3. Tasks — ordered by blast radius, not by yield

Every task: Hypothesis written BEFORE editing; trace with gdb `catch throw` or
`MADC_DT_PROBE`; reducer `tests/testsfinae<name>.mad` + `.flags` (`--std=c++11`)
+ `.expect` from g++ AND clang++ (assert only what both print); ≤3 quick
neighbour runs on the QNAP; then the container: `scripts/remote_build.sh sync
build fulltest` (oracle: green except the four pre-existing failures);
`Searched:` names the concept and the adopted owner. Lane after T1, T4, T7.

- **T0 — reducers banked.** Move the probes to `tmp/declprobe/pending-tests/`
  as `testsfinaedecline`, `testsfinaeabsentmember`, `testsfinaenooperator`,
  `testsfinaeconstructible`, `testsfinaecast`, `testsfinaeaccess` with dual
  oracles; each lands with its task. No tree change.
- **T1 — D1** in `resolve_fn_template_return_by_key`. EXECUTED 2026-09-19 as
  four pieces: (a) `resolve_type_token_range`'s trap mutes and rewinds (the
  throw WAS caught; the rendered+recorded diagnostic was what refused the
  unit); (b) a defaulted TYPE parameter no function parameter names has its
  default substituted in the explicit-args lane too, failure = not viable;
  (c) PARKED as `scratchpad/apply-t1c.py` (T1b): the fallback idiom
  `char (&f(...))[2]` is never REGISTERED (the declarator-name locator knows
  only "name before a top-level `(`"); the draft adds the parenthesized
  declarator-id to the locator, assembles the abstract return declarator in
  the lane and folds it through `fold_template_arg_declarator` — but applied
  now it put sfinae33 / sfinae-nullptr1 OUTSIDE the baseline (their
  `-> char(&)[1]` candidate resolves for the first time and madc's lenient
  operands — a call on a void result, nullptr_t → bool as an implicit
  argument conversion — let its default succeed), and the locator change
  did not reach registration (`g` still undeclared: another classifier
  runs first — trace before T1b). Lands after T3/T6 fix those leniencies;
  (d) latent: `resolve_template_param_default_type` restored its stream but
  not `_cur_token`/`_prv_token` (testexplicitpack regression once (b) made
  defaults substitute on every `declval<T>()`). Interim lane after (a)+(b):
  1441 → 1454. Reducer: k1/k2/k3 shapes
  (absent member, member call on a non-class, undeclared/ADL-failing call) →
  `1 1 1`; negative control: a lone candidate whose substitution fails still
  errors LOUDLY. Lane targets: sfinae7, sfinae42, sfinae48, decltype-nonstatic1,
  and the "use of undeclared identifier" heads. Neighbours: testsfinaealiasoverload,
  testsfinaeconvertible, testsfinaedefscope, testvoidtmembersfinae,
  testfntpldefaultdecltype, testdeclvalreturn, testrecursivedecltypereturn.
- **T2 — D2** absent static member on a concrete class. Reducer: d1 shape →
  `1 2`. Trace first. Neighbours: testtparamnontypedecltype, testdecltypequal.
- **T3 — D4** constructibility predicate (extends the `__is_constructible`
  owner) consulted by `T()`, `T{}`, `new T`. Reducer: k4/k5 shapes (int&,
  void(), abstract, int[], deleted ctor, ND[1], deleted dtor temporary) →
  g++ values. Lane targets: sfinae8/12/15/17/18/21/28, the h<ND> probe.
- **T4 — D5** static_cast validity + sizeof(void/function). Reducer:
  sfinae9's five cases + sfinae38. Lane targets: sfinae9, sfinae14, sfinae38.
  **Lane run here** (interim number for the owner).
- **T5 — D6** access control in unevaluated operands. Reducer: private member
  function + private member typedef. Lane targets: sfinae37, sfinae63.
- **T6 — D3** no viable operator on a class operand is ill-formed. Reducer:
  d2/k6 shapes + positive controls (class WITH operator+; class with
  `operator int()`; `var`/`value` carrier arithmetic). Neighbours: EVERY
  dialect arithmetic test (`grep -l 'var \|value ' tests/*.mad` — run as the
  container fulltest, not here) + testoperatoroverload; **emit-C corpus
  control** (`scratchpad/emitc-corpus.sh` before/after — byte-identical but
  the new test).
- **T7 — close.** Lane + ratchet (`docs/parity/gxx-c++11-baseline.txt` by the
  LOUD list), ROADMAP 2.12 row, `claude_status.json` UPDATE, KG Gap
  `expression_sfinae` → fixed with the measured yield; the reducers ARE the
  gate (no grep gate fits a semantic rule; each reducer carries its negative
  control inside the file).

## 4. Acceptance — measured, not asserted

- Lane: every named target above passes; **0 outside baseline**; the number
  reported is the lane's, on the container, on the committed binary.
- The four probes print the g++ values (`f<B>:1 g<B>:1 h<ND>:1`, `d1<C>:2
  d2<C>:2 k6<C>:1`, k4/k5 → 2 on the ill-formed shapes).
- Fulltest green except testifconstexpr / testinvocable /
  testvecmembercopy_libcxx / testvecpb_libcxx (pre-existing; the seam's).
- Yield estimate (honest): the 24 assertion-firing sfinae tests need T1 +
  one of T2–T6 each; the 19 decltype-family and the decltype-headed
  "undeclared identifier" tests mostly need T1 alone. **+22 is plausible by
  T4; +30 by T7.** A task that moves the lane by zero is DIAGNOSED before the
  next one starts ([[feedback_measure_before_designing]]).

## 5. Out of scope (recorded, next plans)

- ADL inside an unevaluated `decltype` operand: `undeclared_fn(T())` with T
  from namespace N does not find `N::undeclared_fn` (g++/clang++ do). Parked
  reducer `tmp/declprobe/pending-tests/testsfinaeadl.mad`; KG Gap
  `adl_in_unevaluated_operand`.
- `resolve_template_param_default_type` still reads its trailing `*`/`&`/`&&`
  by hand (a default like `class = T(&)[2]` would not fold) — adopt
  `fold_template_arg_declarator` as the range resolver now does (own slice).
- Four isolated-stream owners each save/restore stream + position by hand
  (13541, 14620, 17711, 18125 + the default resolver): a `/dupaudit` family
  candidate, not this arc's.

- `std::initializer_list` completions (probed 2026-09-19: parameter and ctor
  forms WORK, `vector<int>{1,2}` works; missing: `auto x = {…}` deduction (6
  tests), deducing `initializer_list<T>` / `const T(&)[N]` parameters from a
  braced list (7), element conversion in the initializer_list phase
  (`vector<string> v = {"a"}`, ~5), nested braces in ctor argument lists (6)).
  One owner (`respell_braced_list_for_target` + the phase-1 site ~64394) —
  the second +20 slice.
- The 116 "Expecting integer constant expression" tests: constexpr object
  member reads / address constants — the constexpr arc (own plan).
- variadic pack expansion in a parameter-type-list (tsubst spine owner).
- Timeouts auto47.C and ptrmem19.C: profile requests (callgrind on the container).
