# CODEX HANDBACK — local-class identity: keep the 308, make the bound surface green

**For:** Codex (GPT-5.6). **From:** Claude, 2026-07-14.
**Branch:** `feature/forest-sliceA-draingaps-codex` · **Implementation:** `a874bde7` (the carry slice remains preserved at `dd14e76d`).
**Verdict:** **LANDED 2026-07-14.** The local-class carry stays enabled, and deterministic hoisted-declaration identities make the live, frozen, and bound surfaces agree.
**Prior briefs:** `docs/plans/2026-07-13-CODEX-HANDOFF-family-D-drain-gaps.md` (campaign context, metric, tools) — still applies in full.

---

## LANDED RESULT

- Local classes, GNU nested functions, and dependent-pattern functions now use
  stable identities derived from the enclosing emission symbol, source name,
  and a per-body declaration ordinal. The global `nested_counter` and
  `pat_counter` mint sites are gone.
- Pattern-local classes retain their source identity through tsubst and are
  remapped to the concrete enclosing function. Computed-name collisions fail
  loudly instead of acquiring a counter suffix.
- `tests/testlocalclassidentity.mad` pins two same-named local classes in
  sibling scopes. The live and packed binaries both print `15`, matching the
  GCC and clang reference lowering; `testlocalclassraii` remains parse-once at
  1 hit / 0 fallback.
- Reducer freeze: **308** pack drops. `make -C src fulltest`: rc=0,
  **695 passed / 0 failed / 0 timed out / 16 skipped**, including `[subbind]`
  and every forest oracle. `make -C src release`: rc=0, 240 units packed.
  Packed release suite: **695 / 0 / 0 / 16**, rc=0.
- KG `Decision {local_class_forest_carry}` is `landed` as of 2026-07-14.

---

## ⚡ THE CONTRACT (owner's words: BOTH live and frozen must have no regressions)

`/goal` — falsifiable end-state, ALL of:
1. Reducer-corpus pack drops **≤ 308** (the carry win is kept — a fix that reverts to the exclusion is a non-answer).
2. `make -C src fulltest` **rc=0 run to completion** — integration 694+/0/0/16 AND every forest gate green, **including `forest_bind_gate [subbind]`** (currently RED: "bind output differs from live").
3. `make -C src release` rc=0.
4. `MADC_BIN=bin/madc-release bash scripts/run_tests.sh` — **full packed suite green** (currently **640/54 FAILED**).
5. A live test pinning the fixed shape (local class in a drained body, bound and executed == g++) if one doesn't already exist that covers it.

**Validation law (round 1's lesson, non-negotiable):** `--run-frozen` and single-test spot-checks do NOT validate the bound surface — `--run-frozen` compiles the pack's own tree-resident defs, so pack-context symbols always resolve there. Only grove-BIND consumers (`forest_bind_gate`, esp. `[subbind]`) and the FULL packed suite exercise restore-into-consumer. Run all three legs TO COMPLETION before claiming anything. Never interrupt a gate to "preserve a handoff" — commit WIP first, then validate.

---

## THE EXACT GAP (evidence — reproduced and root-caused, do NOT re-derive)

```
$ bin/madc-release tests/testcontainerdtor.mad
MIR error: import of undefined item __pat97__basic_string_char_std__char_traits_char__std__allocator_char____Guard___Guard__17
  undefined MIR import: __pat97__..._Guard___Guard__17     (the _Guard ctor)
  undefined MIR import: __pat97__..._Guard___dtor__18      (the _Guard dtor)
```
54 packed tests fail with this class of signature (string/lambda/container families);
`forest_bind_gate [subbind]` diverges the same way.

**Why:** a frozen enclosing body (e.g. `basic_string::_M_construct`, whose body contains
the local class `_Guard`) calls its local class's hoisted methods by a symbol composed
from GLOBAL PARSE-ORDER COUNTERS. A bound consumer restoring that body can never
resolve the import — and its own derivation of the same source mints DIFFERENT
counter values. The pre-existing exclusion you removed was papering over exactly
this: local classes had **no stable cross-context identity**. Carrying the records
(your slice) is necessary but not sufficient — the SYMBOLS must be re-mintable.

**The unstable mint sites (both are the bug):**
- `src/parser.cpp:1615` — `make_nested_function_name()`: hoist name =
  `<owner>__<local_name>__<++nested_counter>` where `nested_counter` is a global
  static — pure parse-order. This is the `__17`/`__18` in the failing imports.
- `src/parser.cpp:36851` — pattern parse id = `fd->name + "__pat" + pat_counter++`,
  another global static — the `__pat97` component. A local class created during a
  pattern-context parse embeds this id in its identity chain.
- ⚠️ Read the comment block at `src/parser.cpp:~36880-36888` before touching
  pattern-context naming: there is a KNOWN hijack subtlety (testlocalclassraii —
  a pattern body's local class must keep caller-scope-dependent naming or it
  hijacks the shell parse's real one). Your fix must keep that test green.

## THE TASK — deterministic local-class identity

Make every hoisted local-class (and nested-function) symbol a **pure function of
stable source identity**, so pack-time freeze and consumer-time derivation mint
the SAME name. The forest invariant is the spec: **LOADED must EQUAL PARSED** —
same source, same symbol, in every context.

Design requirements (mechanism is yours to choose; these are the constraints):
- Key on things stable across contexts: the ENCLOSING definition's emission
  symbol (the mangled/instantiated name — already order-independent since the
  `overload_spelling_symbol_suffix` work), the local class's source name, and a
  **per-enclosing-body ordinal** (declaration order within that one body — stable
  because the body's token run is identical wherever it is parsed). NEVER a
  global counter, never a pattern-instance id (`__patN`) for something a
  consumer re-derives outside pattern context.
- Two same-named local classes in different scopes of one function must still
  get distinct, deterministic ordinals (declaration order covers this).
- The DEFBODY-derive path on the consumer side must reproduce the name — that
  is the definition of done for identity. If pattern-context and plain-context
  parses of the same enclosing body would mint differently, the identity is
  wrong (see the 36880 comment for the one deliberate exception and keep its
  test green).
- Collision guard: if a computed name can collide (hash or ordinal), fail LOUD
  at freeze time — never silently uniquify with a counter again (that
  reintroduces the bug).
- After the naming change, RE-FREEZE everything you measure against (all msnap
  corpora are transient in `tmp/`; the release pack re-freezes in
  `make -C src release`). Stale snapshots mixing old and new names will produce
  phantom results.

Suggested probe while developing: `MADC_CHECK_ATTRIB`, the named pack check gate,
and a tiny two-context experiment — freeze a TU containing a local class, then
bind it from a consumer and diff the minted symbols (that diff being empty IS
the fix).

## GATE (before you claim done — no exceptions, no interruptions)
```bash
timeout 120 bin/madc --freeze=tmp/_bfNEW.msnap tmp/_bf3.cpp > tmp/bfNEW.log 2>&1
grep -c "pack drop" tmp/bfNEW.log          # must be <= 308
make -C src fulltest                        # rc=0, ALL forest gates incl [subbind]
make -C src release                         # rc=0
MADC_BIN=bin/madc-release bash scripts/run_tests.sh   # FULL green — THE arbiter
```
Then: commit (scoped files only — never `git add -A`; `mir-debug-support.md` is
foreign), update the plan-doc stamp + CHANGELOG + `claude_status.json`, flip KG
`Decision {local_class_forest_carry}` from `validation_failed` to `landed`
(`bash scripts/kg_query.sh`), push. Merge to `develop` only with everything
above green — the ladder is 121+ below the 429 merge criterion, so a green
matrix here IS the merge trigger for the whole family-D campaign.

## SETTLED — DO NOT RE-LITIGATE
- **We never throw away work** (owner). Your carry slice stays; the fix goes
  UNDER it. Round 1's WIP commit `dd14e76d` is the base.
- Forest invariants: LOADED == PARSED; no re-parse at load; NO new record
  families. Deterministic naming changes SYMBOLS, not record kinds — in bounds.
- No name-keyed special cases (`"_Guard"`, `"basic_string"` string matches are
  wrong by fiat). Key on structure: enclosing-symbol × source-name × ordinal.
- The packed suite is the arbiter; the check gate and `--run-frozen` are not.
- `stash@{0}` ("codex-preserve-member-template-wip") regressed
  `testcontainerdtor` pre-stash — leave it alone unless you are deliberately
  resuming that work AFTER this lands.
