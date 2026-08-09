# Class-pattern value arguments — the USE-side wall burn-down (task #25 follow-on)

## Mission (owner directive 2026-08-08)

testsubscript.mad (4 container types) costs ~425ms wall vs g++ -O0's
~325ms — madc is 30% SLOWER than gcc despite pre-parsed headers (a
gcc-parity pathology). 280 of ~500 class instantiations re-parse live.
Owner: "there must be something else inefficient going on" → confirmed;
"start on it" → this plan.

## Root cause (recon 2026-08-08, all verified by probe)

The class-pattern lane refuses exactly the shapes libstdc++'s internals
are made of, at FOUR fences:

1. **Doom pre-filter** (parser.cpp ~8267): a template with ANY non-type
   param (`enable_if<bool,T>`) or pack param (`__and_<..._Bn>`) is
   stamped doomed and NEVER captured. With live capture ON, the doom
   persists via writeback on first demand — behaving as designed; the
   design boundary is the problem.
2. **Dependency poisoning** (`dependent_shell_fallback_reason`
   ~27854): a pattern whose body references a template-id SHELL whose
   target has non-type params is failed at capture with
   DependentValueExpression — because `pattern.types[id].arguments`
   are ClassTypePatternIds (TYPE slots only); a VALUE argument cannot
   be encoded. This is what poisons `std::pair` (deps `std::_PCC`,
   `std::__conditional` — probe: `shell-fallback: pair dep=std::_PCC
   reason=5`, fail at parser.cpp:28266) even though its capture
   otherwise SUCCEEDS (2836 pattern words in the forest, reason=5).
3. **Eligibility gates** (basic_class_pattern_eligibility ~5768):
   `has_non_type_params || !token_subst.empty()` → refuse;
   `!pack_subst.empty()` → refuse; `static_members/static_values`
   non-empty → refuse (5808).
4. **Pack-time coverage**: the trait families (enable_if, __and_, …)
   have NO template records in the forest at all — the producer never
   parsed their definitions (decl-index rows only, e.g. `enable_if
   [120,134)`); consumers re-parse the decl live, then hit fence 1.
   Live capture (`MADC_CLASS_PATTERN_LIVE`) is env-gated OFF by
   default, so consumers never self-heal.

Also verified: `class_pattern_use_count` never advances for doomed
templates (doom branch precedes counting — by design, not a bug);
`static_values` in ClassAggregatePatternNode already stores folded
(name, int64) pairs — the structure for static value members EXISTS,
only eligibility + serve replay are missing.

Cost model (testsubscript tally): 51× __and_ [pack], 20× enable_if
[non-type], 16× __or_ [pack], 10× integral_constant [static value +
non-type], 10× __is_nothrow_swappable_impl, 9× _PCC, 9× pair
[dependency poisoning], 9× __not_, ~30× is_*_able family — of 280.

## Slices

- **N1 — ValueArg in pattern TemplateId dependencies.** New
  ClassTypePattern argument form carrying a non-type argument's TOKEN
  RUN (folded via fold_nontype_arg_constant at serve after param
  substitution; the spelling arm renders the folded value into the
  dependency's rebuilt spelling — check whether TemplateId deps resolve
  purely by spelling in basic_class_pattern_type_spelling; if so the
  serve side is nearly free). Relax fence 2 to "value args must
  normalize/fold". Unlocks pair, and every pattern currently poisoned
  by a _PCC/__conditional/enable_if dependency.
- **N2 — own non-type params.** Relax fences 1+3: attempt capture; let
  capture's own normalize fail() catch bodies that genuinely embed
  value expressions it can't encode. A value param unused in the body
  (enable_if primary `{}`) serves trivially; spec-pattern selection
  already folds values (match_partial_specialization +
  fold_nontype_arg_constant). token_subst non-empty stops being a
  blanket refusal — it feeds the serve's value bindings.
- **N3 — static value members.** Eligibility admits
  static_members/static_values; the serve replays them (register the
  folded constant — resolve_class_static_member_const_value is the
  read side). Unlocks integral_constant + every trait deriving from it
  IF the initializer folds (the __is_* builtins do).
- **N4 — pack params** (__and_/__or_/tuple): pattern capture/serve for
  variadic class templates. Biggest; separate design pass.
- **N5 — coverage + default-on.** Pack-time: force-parse + capture the
  high-demand trait families (or capture on decl-index resolution);
  decide defaulting MADC_CLASS_PATTERN_LIVE on (owner call — it
  changes live-lane behavior/perf). Re-measure testsubscript vs g++.

## N1 recon (2026-08-09, verified by reading)

- **Serve side is spelling-driven — confirmed.** TemplateId dependencies
  rebuild as `type.name + "<" + arg-spellings + ">"` in
  `basic_class_pattern_type_spelling` (parser.cpp ~5515); every argument
  slot recurses into the same spelling builder. A ValueArg kind only has
  to render its value text into that string.
- **Capture insertion point:** the TemplateId argument loop
  (parser.cpp ~28060-28105) splits args on DelimDepth and recurses each
  through `normalize_token_type`, which fails on value expressions
  (UnnormalizableType). The ValueArg arm goes there, keyed by the head
  target's `typeparam_is_type[slot]` (same per-slot discipline as
  slice N2's eligibility).
- **Fence 2 relaxation:** `dependent_shell_fallback_reason`
  (parser.cpp ~27876) currently poisons on `has_non_type_template_parameter`
  of the dependency's target ALONE. After ValueArg lands it must only
  poison when a value slot failed to capture.
- **Struct fields reuse — no new fields needed:**
  `ClassTypePatternKind::ValueArg` appended LAST (existing serialized
  ordinals stable); `name` = source spelling of the expression
  (`class_pattern_tokens_spelling` already exists and is used by the
  fallback arm); a `flags` bit marks "pre-folded at capture" with the
  value in `dimensions[0]`.
- **fold_nontype_arg_constant(tokens, int64&)** — the existing fold
  owner (parser.cpp 4666/4781/4865/4978/7547/7799 call shapes).
- **RESOLVED — the serve consumer is the resolver, not the spelling.**
  The concrete-spelling mode has NO serve-side caller; eligibility uses
  concrete=false only. The real consumer is
  `BasicClassPatternResolver::resolve`'s TemplateId arm (parser.cpp
  ~6429): `instantiate_pattern_template(type.name, type.arguments, ...)`
  resolves each argument ClassTypePatternId to a DataDef* and routes
  through the FULL instantiation dispatch (with memo: memo_arguments is
  a vector<DataDef*> feeding the resolution hash).
- **N1 serve design (decided):** the full dispatch already evaluates
  value arguments from TOKENS in the live lane (fold_nontype_arg_constant
  after substitution) — so ValueArg serves by handing its captured token
  run (params substituted per binding.type_subst) to that SAME dispatch.
  Parse-once compliant: tokens saved once at capture, resolver re-runs
  over the saved run — the tsubst model, no re-lex/re-parse.
  Implementation surfaces: (a) ValueArg token-run storage — patterns
  store runs as plain `std::vector<TokenBase *>` (body_tokens,
  template_param_defaults, param_type_token_runs) which the freeze
  serializer already persists; give ClassPattern ONE side table
  `std::vector<std::vector<TokenBase *> > value_arg_tokens` and let the
  ValueArg node reuse `template_param_index` as its index into it
  (`name` = source spelling for diagnostics/canonical spelling) — keeps
  ClassTypePattern lean and the serializer delta to one field;
  (b) instantiate_pattern_template grows a value-slot transport
  (folded int64 per value slot after substitution); (c) memo hash must
  incorporate value slots (memo_arguments is DataDef*-only today);
  (d) pre-fold constant expressions at capture (flags bit +
  dimensions[0]) to skip serve-time work for `true`/`0` literals.

## Facts / probes for the next session

- Probes: `MADC_CLASS_PATTERN_PROBE=<substr|*>` (lazy-arm on STDOUT;
  eligibility REJECT / thaw / capture OK / normalize FAIL sites= on
  stderr&stdout — capture window mutes cerr, those go to cout);
  `MADC_USE_COUNT_PROBE` (needs -DMADC_USE_COUNT_PROBE build; probes
  in note_class_pattern_use + registered_template_entry_for).
- Live-capture A/B on packed: `MADC_CLASS_PATTERN_LIVE=1` — counts
  unchanged on testsubscript (all 280 are doomed families).
- Baselines (packed, best-of-N): testsubscript total ~395-450ms;
  string probe 124ms; g++ -O0 same source ~325ms warm.
  `class instantiate: 220 pattern / 280 parse / 642 cache / 67 opaque`.
- Equivalence gates: scripts/class_pattern_equivalence.sh (fulltest);
  force_legacy_class_patterns / MADC_CLASS_PATTERN_FORCE_LEGACY = the
  A/B lever; the [why:] tally on testsubscript is the burn-down metric.
- Branch: feature/class-pattern-nontype-claude off develop v0.73.0.
- parser.cpp carries two #ifdef MADC_USE_COUNT_PROBE diagnostic blocks
  (note_class_pattern_use, registered_template_entry_for) — env+ifdef
  gated, keep.
