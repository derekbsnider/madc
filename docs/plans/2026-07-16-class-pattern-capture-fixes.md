# Class-pattern capture/restore fixes — B3 regression takeover (Claude, 2026-07-16)

**Context:** Both Codex sessions hit usage walls mid-B3 (salvages `5ad4efc6`,
`89813d5b`). Owner asked for a full review of B0→B3 and a direct fix attempt.
This doc records what the review found, what was fixed, the measured state,
and what remains. Companion commits on `feature/class-parse-once-codex`.

## What the review established (three agents + inline verification)

**The regression's anatomy (per slice):**
- **B0** — faithful extraction refactor; census/counters initially always-on
  (unfixed until the successor gated them behind `--show-stats`).
- **B1** — the live killer: eager capture at EVERY template definition = one
  full production-parser body parse + a whole-type-table walk
  (`class_pattern_collect_stable_types`) + a ~35-container deep-copy journal
  per definition, whether or not the template was ever instantiated. The
  "+1,863 instantiate calls" were capture parses + their opaque-shell
  resolutions (double-counted `InstTimer` entries). B1's "zero
  instantiation-path change" was validated against the class counters, which
  stayed flat while the raw `instantiate` bucket ballooned — wrong counter.
  Bind-side: eager decode of ALL restored patterns.
- **B2/B3** — pattern lane + widening; the resolver re-enters the full
  instantiation dispatch per dependency with a per-call memo (sibling
  aggregates re-derive the same chains); nested-template re-registration
  pushes a full TemplateDef copy into `template_map` per outer pattern
  instantiation (unbounded variant growth + linear rescans).
- **Successor session** — five real perf fixes (O(1)
  `speculative_class_capture` flag; true transactional journal +
  `id_table` transactions; lazy bind-side pattern materialization 33/532;
  `find_readonly()`; `definition_origin` precompute), call-site counters,
  census gating — and one design retreat: live capture gated to
  pack-time/TU-root only (live lane dead by construction, unit-tested as
  intended).

**The part nobody had seen:** even on the bound leg the flagship templates
(`std::vector`, `std::map`, `std::set`, `_Vector_base`, `_Rb_tree`) never used
the pattern lane — their patterns captured COMPLETELY (vector: 8,340 payload
words) yet carried failure reasons. Five stacked defects:

1. **Member-template capture lookup.** A member template's capture parse
   registers its shell under the owner-mangled key (`Owner__<identity>` —
   TokenCLASS::parse nested-name rewrite at parser.cpp:31936); capture looked
   up the bare identity → "no DataDefSTRUCT registered" → 26 root failures
   (`rebind` ×26, `__not_overloaded` family). Fixed: consult the
   owner-mangled key too.
2. **Reason contagion.** `dependent_shell_fallback_reason` returned the
   DEPENDENCY's own `class_pattern_reason` — one deep unnormalizable
   dependency (`__alloc_traits`' `_Alloc::rebind` chain) poisoned every
   pattern referencing it, transitively (28 poisoned incl. all flagships).
   Fixed: a dependency's capture state is not contagious — the resolver
   routes TemplateId dependencies through the full dispatch, which picks
   that dependency's best lane per binding.
3. **Capture-time member-guarantee gate.** `PatternMaybe<T>::value_type`-style
   checks required the dependency's pattern to answer "does it define member
   X" and required reason==None to consult it — the same contagion one layer
   deeper, and unable to see through member-template chains
   (`__alloc_traits<...>::rebind<...>::other` blocked every container).
   Deleted: after substitution every source is concrete; the resolver walks
   member chains through the machinery the parse lane uses; a genuinely
   absent member errors at instantiation with the parse lane's diagnostic.
4. **Pattern payload type-id swizzle.** Concrete types serialized the
   producer's RUNTIME type id raw; at bind the id dangled (project-segment
   ids are process-local) → eligibility rejected every such binding
   (`concrete type[23] id=16777331 resolved=0`). Fixed: the payload reader
   swizzles through the forest's persisted tid→DataDef map
   (`CirFrozenForest::restored_def_by_tid`) and re-mints the consumer's id.
5. **Non-portable semantic fingerprint.** `class_pattern_fingerprint` hashed
   the raw type id, so a correctly-swizzled restore could never verify (and
   the old "passing" reads were verifying dangling ids). Fixed: hash the
   concrete type's canonical spelling. NOTE: this invalidates the stored
   fingerprints of every pre-fix `.msnap` — re-freeze corpora.

## Measured state after the fixes

- **Pack census (full 240-unit header TU):** clean patterns 351 → **434**;
  root capture failures 181 → **128**; poisoned-with-pattern 239 → 188
  (remaining are honest per-KIND gaps: 157 dependent-value-expression from
  non-type-param dependencies, friends, etc.).
- **Bound testsubscript (dev `--forest-bind`, same corpus):**
  54 pattern / 199 parse / 338 cache → **77 pattern / 160 parse / 291 cache**;
  `std::vector<int32_t>` (B3's flagship case) instantiates through the
  pattern lane; `std::map` now blocked only by its honest
  `unsupported-friend-definition` (B6 KIND); live-vs-bound output
  byte-identical.
- **Live (dev -O0, medians of 5, quiet host):** default (lane off) 2.22 s ≈
  legacy 2.18 s — parity, the B0–B3 always-on taxes are gone.

## Lazy live capture (opt-in: `MADC_CLASS_PATTERN_LIVE=1`)

The successor's live amputation is replaced by demand-driven capture:
header templates defer capture to their SECOND concrete demand (first demand
arms a counter on the registered definition via
`note_class_pattern_use`/`writeback_class_pattern_capture`, matched by
body-token identity). A definition-only pre-filter skips categorically
ineligible templates without parsing (non-type/value/pack params, friend
bodies — the reasons eligibility rejects unconditionally). A cycle guard
(`class_pattern_inst_in_progress`) returns the early-registered incomplete
shell on re-entry — the parse lane's self-reference semantics — instead of
recursing to stack overflow (the resolver's dependency web is cyclic on
live; bound never saw it because restored state pre-satisfies lookups).
Capture nested inside a pattern-lane instantiation is refused (journal depth
gate): a nested isolate journal does not snapshot, so its rollback could not
undo the capture parse.

**Why it defaults OFF:** measured on live testsubscript, engaged lane =
2.30 s vs 2.18 s legacy. With captures cut to 74 (demand threshold 3) the gap
stayed ~+100 ms — so the residual cost is the pattern lane's
PER-INSTANTIATION overheads, not capture:
1. nested-template re-registration churn (full TemplateDef copy into
   `template_map` per outer pattern instantiation; variants never match →
   unbounded growth + linear `find_template` rescans),
2. journal open/close walking every `namespace_map` entry per outer hit,
3. resolver re-derivation (fresh memo per top-level call; sibling aggregates
   re-derive shared chains through the full dispatch).
When those land (B4+ material), flip `class_pattern_live_capture` on by
default and re-measure — the machinery is correct, tested, and one knob away.

## Diagnostics added (env-gated, zero cost unset)

`MADC_CLASS_PATTERN_PROBE=<substr>|*` prints through the whole pipeline:
capture OK/FAILED (with the swallowed error detail — capture mutes cerr and
rolls back diagnostics, which is why these defects were invisible),
normalize-fail sites (`__builtin_LINE`), freeze-emit records, restore
records, lazy materialize results (incl. payload-reader fail line),
eligibility rejects with sites. Probes inside the capture window print to
cout (cerr is nulled there).

## Remaining work (next session's worklist)

1. **Per-instantiation pattern-lane costs** (the three mechanisms above) —
   this is what blocks live default-on AND further bound gains.
2. **B4–B6 per the parent brief** — string closure, map/set/list closure
   (map/_Rb_tree need the friend KIND — `_Rb_tree` reads reason=8 honestly
   now), reason burn-down (157 DVE family = non-type-param dependencies need
   value-argument representation in patterns).
3. **Packed-leg + bench validation gates** as always: packed suite + blob
   presence + bind gate + TSV row at load<2.
4. The `semantic_fingerprint` of patterns whose types resolve differently
   across producer/consumer contexts is now the honest restore gate — watch
   its failure rate via the materialize probe if bound pattern counts sag.
