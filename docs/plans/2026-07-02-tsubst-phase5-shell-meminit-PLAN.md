# Phase-5 — shell / ctor mem-init tsubst → DELETE the re-parse path

Status: SCOPED (recon 2026-07-02, grounded file:line). Successor campaign to the
default flip (`a2262c35`, handoff UPDATE-6). Campaign law: `.claude/rules/parse-once.md`.
Parent plan: `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` §11.3(B)
("the shell parse can later be made copy-based too — a follow-on") + Phase 5.

## Why this blocks deletion

The burndown (268 hit / 0 fallback) means no BODY **lowering** uses the re-parsed
tokens. But under hybrid B the instantiation re-parse still RUNS for every
instantiation: it produces (a) the concrete signature/shell the parser needs at
parse time (§11.2 core tension), (b) `FuncDef::ctor_initializers` — mem-init
ARG TOKEN TREES (`include/madc.h:235-239`) parsed by `parse_ctor_initializer_list`,
and (c) the body tokens whose parse product the tsubst hit then discards.
Deleting the re-parse machinery therefore requires the mem-init semantics (b)
to come from Tree-1, and the body parse (c) to be skippable.

## Grounded mechanics (recon 2026-07-02)

- Mem-inits lower in `func_def`'s ctor PROLOGUE (`cir_builder.cpp:15385-15440+`):
  `find_base_initializer` / `find_member_initializer` walk `fd->ctor_initializers`
  and lower `ci->args` (TokenBase trees) via `class_ctor_call_addr` etc. — this is
  INDEPENDENT of `tsubst_method_body` (which replaces only the `{}` body node).
  So today a "hit" ctor takes its body from Tree-1 and its mem-inits from the
  concrete shell's re-parse. (This is how the empty-body pair piecewise/indexed
  ctors are hits: all semantics are shell-side.)
- The PATTERN parse (`build_dependent_pattern` → `parseFunction`) already runs
  `parse_ctor_initializer_list` when the retained decl carries `: inits` — the
  pattern fd gets dependent `ctor_initializers` (args referencing placeholders /
  pack expansions). UNVERIFIED: whether the pattern fd's ctor_initializers
  survive today or are discarded with the pattern parse — check first.
- Pattern granularity: patterns are keyed per concrete-owner member-template
  FuncDef (`fd->dependent_pattern`), so the OWNER (members, bases, layout) is
  CONCRETE inside a pattern. Only the member template's own params are
  placeholders. This makes mem-init pattern-ization tractable: base/member
  resolution in the prologue needs no substitution — only the ARG EXPRESSIONS do.

## Slices (each gated: fulltest + ratchet + burndown flat + sweep; torture when
flag-off-reachable)

1. **Mem-init Tree-1**: at pattern build, lower the pattern fd's ctor_initializers
   into the Tree-1 pattern (pattern-mode arg lowering; prologue-shaped nodes
   attached ahead of the body, or a parallel per-ci node list on the pattern).
   At `tsubst_method_body` hit for a ctor, emit the substituted mem-init nodes
   and have `func_def`'s prologue SKIP its token-side emission for exactly the
   inits the pattern covered (whole-ctor switch, not per-init mixing).
   Start with the pair piecewise/indexed ctors (empty bodies — the pattern IS
   the mem-inits) and `_Rb_tree::_Auto_node`'s ctor.
2. **Body-parse skip**: for a method whose fd has `tsubst_source` +
   `dependent_pattern` and whose instantiation would be a hit, make the
   instantiation parse skip the body tokens (the deferred-body machinery
   already skips bodies at class parse — extend `parse_deferred_lazy_body` /
   the CIR fixpoint to try tsubst BEFORE materializing the lazy body, falling
   back to materialization only when tsubst bails). Measure parse-time win
   (--show-stats): this is the O(n²)-class front-end cost the law cites.
3. **Bail = error**: once slices 1-2 soak green, a tsubst bail on a covered
   shape becomes a LOUD error instead of a fallback (the bail-net's
   swallow-and-reparse behavior dies first).
4. **DELETE**: remove the re-parse instantiation body path + the =0 escape
   hatch + `madc_tsubst_dep_parse_enabled()` + the unit-test fallback-counter
   specimen (test_cir.cpp engagement counters — replace with a hit-counter
   assertion). Build with -Wall; `-Wunused-function` on the deleted web
   confirms the cut (no-parallel-implementations).

## Open questions to answer before slice 1 code

- Does the pattern fd retain parsed `ctor_initializers` after
  `build_dependent_pattern` returns? (The failure path erases funcdef_map
  entries; the success path moves the TokenFunc to fd->dependent_pattern —
  where do the fd-level ctor_initializers live?)
  **Partial recon (2026-07-02):** the pattern FuncDef object survives via
  `pattern->var.type` (only the funcdef_map ENTRY is erased on success,
  parser.cpp:34311). BUT parseFunction accepts `: inits` only when
  `parsing_defaulted_member_template_constructor` is set (parser.cpp:38057-38081
  gate) — set ONLY by parse_deferred_lazy_body's full_definition branch — or
  when the id resolves as `<owner>__<ctor>[__oN]`; the pattern's `__pat<N>`
  rename defeats the ctor-tail resolution. So a ctor pattern parse today most
  likely does NOT take the mem-init path — PROBE how pair's piecewise ctor
  (an empty-body HIT) actually parses its `: pair(...)` span in the pattern
  (does the `:` throw and the pattern still build because...? or is the
  mem-init span stripped before def_tokens?) before designing slice 1.
  ~~First slice-1 step may simply be: set
  `parsing_defaulted_member_template_constructor` (or a pattern-mode
  equivalent) across the pattern parseFunction so the pattern fd's
  ctor_initializers populate.~~
  **PROBED 2026-07-02 — the one-flag version CRASHES; both questions answered:**
  - An env-gated probe at build_dependent_pattern's tail confirmed: the
    mem-init span IS present in a ctor pattern's def_tokens (pair o19:
    `colon_before_body=1 parsed=1`), the pattern parses today ONLY because
    the parser.cpp:38111 skip loop silently DISCARDS the `: inits` span
    (`parse_ctor_initializers` false — the `__pat<N>` rename defeats the
    ctor-tail detection), and `ctor_initializers` is 0 on EVERY pattern fd.
    Also: many never-ODR-used variants' patterns fail to parse (parsed=0 for
    char16_t/char32_t/pmr basic_string ctors) — invisible to the burndown
    because nothing instantiates them.
  - Setting `parsing_defaulted_member_template_constructor = true` across the
    pattern parseFunction (the lazy full-definition precedent) SIGSEGVs
    testmap: unbounded recursion in `TokenCpnd::findVariable` walking a
    compound parent chain (repeated `findVariable+0x168` frames) — the EAGER
    mem-init arg parse runs BEFORE the function's body compound exists and
    re-enters instantiation machinery mid-pattern-parse. The body parse's
    dependent deferral (`dependent_parse_in_progress`) did not prevent it, so
    mem-init args hit an instantiation path that ignores that flag — find it
    before retrying (the backtrace signature is the search key).
  - **Candidate slice-1 design (grounded in existing machinery):** do NOT
    parse mem-inits eagerly in the pattern. Capture the raw span the way the
    class-body sink path already does (`pending_deferred_ctor_inits`,
    parser.cpp:38122+/38139+) onto the pattern fd, then REPLAY it inside the
    pattern's body compound the way `parse_deferred_function_body` replays
    `ctor_init_tokens` ahead of the body (parser.cpp:24162-24173 +
    parse_ctor_initializer_list at 24222) — [class.base.init] complete-class
    context, and the args then parse in the same compound context as the
    body, where the dependent deferral machinery is known to work.
- Prologue ordering: member inits interleave with base ctors and vptr stores
  in DECLARATION order ([class.base.init]) — the pattern-side nodes must
  reproduce that order or delegate ordering to func_def (prefer: pattern
  carries per-ci arg NODES, func_def keeps ordering/default-init logic and
  consumes substituted arg nodes instead of tokens — smallest change, keeps
  one ordering implementation).
- Delegating ctors (`find_delegating_initializer`) — same treatment, arg
  nodes from pattern.
