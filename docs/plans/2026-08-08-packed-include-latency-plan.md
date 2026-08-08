# Packed-lane `#include <string>` latency (task #25, 2026-08-08)

## Mission (owner directive)

"Nearly 200ms just to use a std::string seems like an awfully large
overhead… `#include <string.h>` the C way takes about 1/4 the time…
figure out how to lower std::string down a bit more so the difference
isn't so huge."

## Baseline (container, `bin/madc-release`, `--show-stats`)

| probe | total | units bound | register | node-record segs | zstd decoded |
|---|---|---|---|---|---|
| bare `int main(){}` | 2ms | 0 | — | 0 | — |
| `<string.h>`+`<stdio.h>` used (C way) | **31ms** | 30 | 1ms | **0** | 15MB |
| `<string>` UNUSED | 154ms | 139 | 32ms | 6 | 57MB |
| `<string>` + one std::string use | **185ms** | 139 | 32ms | 12 | 76MB |

Phase buckets lie today: release "lex" = 96ms at 266 tok/s (forest
bind/restore under the lexer's `#include` window), and ~45ms of
on-demand unit loads land inside "cir build" (84ms shown; real lowering
~35-40ms per the dev-binary comparison).

## Profile (callgrind, unstripped -O2 packed twin at tmp/madc-release-sym; unused-include probe, 779M Ir)

| share | what | root cause (source-verified) |
|---|---|---|
| 12.8% | zstd decompress | 57MB decoded at bind for zero uses |
| 12.1% | `__memset` | `std::vector::resize` zero-fills BOTH the staging buffers (`rb/kb/cb/pb`, `unit_segment` cir_freeze.cpp:3553) AND the typed destination vectors, all fully overwritten right after — payload memset once, copied twice |
| 6.1% | `CirFrozenForest::unit_segment` self | first-load validation loop over every record+child (the corrupt-container firewall — keep) |
| ~14% | malloc/_int_free/consolidate | staging-buffer + token/record allocation churn |
| 4.7% | `__memcmp` | string-keyed map compares (register/intern paths) |
| 3.4% | `madc_pch::deserialize_tokens` | template pattern TOKEN RUNS deserialized eagerly at restore |
| 2.4% | `materialize_from_arena` | typed-DataDef rebuild |
| ~4% | `findVariable`/`intern`/`_Rb_tree` insert | eager registration into live parser maps |

Also source-verified: `snapshot_reader::find()` (madcdis_snapshot.cpp:380)
is a LINEAR scan of the segment directory per lookup (4 finds × unit).

## Profile refresh (post-A/D, tmp/cg_string_postAD.out, string probe, 878M Ir)

| share | what | disposition |
|---|---|---|
| 13.4% | zstd | mostly UNIT node-record loads (12 units ≈ 3.2MB/unit), not templates — needs a pack-side lever (slice E candidate: faster codec level / hot-cold split for node segments) |
| 12.9% | `__memset` | the ONE remaining resize-zero-fill per decode destination (columnar `rb`, children/positions, thread-local planes, every aux `read_segment(out)`) → **slice A2** |
| 8.3% | `unit_segment` self | per-record validation firewall (445k records when all load); candidate: trust the SELF-IMAGE arm (owner decision) |
| ~12.5% | malloc/free churn | deserialize TokenBase allocs (slice B) + map/vector churn |
| 4.4% | memcmp | string-keyed compares (slice C domain) |
| 3.0%+1.4% | deserialize_tokens + token_from_id/TokenBase::new | eager template restore_run → slice B (~10ms, smaller than first modeled) |
| 2.1% | materialize_from_arena | slice B (template walk share) + C |
| ~3% | findVariable family | registration lookups → slice C |

## Profile refresh (post-B, tmp/cg_string_postB.out, string probe, 738M Ir — down 16% from post-A/D's 878M)

| share | what | disposition |
|---|---|---|
| ~17.0% | zstd (15.9 + 1.1) | unit node-record loads, 76MB/12 units — NOW THE #1 LEVER: slice E (pack-side codec level / hot-cold split for node segments; pack-format change) |
| 9.9% | `unit_segment` self | per-record validation firewall — trusting the SELF-IMAGE arm = OWNER decision |
| ~12.7% | malloc/_int_free/free | token + DataDef alloc churn (diffuse: deserialize, materialize, maps) |
| 5.8% | `__memset` | residual (A2 killed decode destinations; likely calloc/arena zeroing outside the decode path — not yet chased) |
| 4.9% | memcmp | string-keyed map compares — slice C domain |
| 2.4% | materialize_from_arena | typed-DataDef rebuild |
| 1.8% | deserialize_tokens | remaining eager: MEMBER/OOL staging + thawed defs (down from 3.0%) |
| ~2.7% + 1.4% | findVariable family + _Hash_bytes | registration lookups — slice C domain (register bucket now 21ms) |

Slice C's whole domain is ~9% ≈ 21ms; zstd + validation together are
~27%. Both bigger levers need decisions C doesn't: E changes the pack
format, the validation arm trades the corrupt-container firewall for
speed on the self-image. **Milestone decision (2026-08-08): close the
branch at slice B (merge + release), take C/E on a fresh branch after
the owner weighs in on the E format change and the validation arm.**

## Why an unused include pays at all (the eager-restore chain)

`forest_restore_decls` → template-record walk (cir_freeze.cpp ~3294):
every template whose declaring unit is in the bound closure and whose
key passes the demand verdict calls `ensure_template_payload()` — which
decodes the payload/token segments and builds `CirRestoredTemplate`
(key, name, param runs, token runs) for EVERY such template. For
`<string>` that is hundreds of surviving templates, none of which the
program may ever instantiate. The laziness is per-TU ("trivial C never
pays"), not per-template. The C path is fast precisely because it has
no templates to restore and libc functions ride the dlsym fallback
(register 1ms vs 32ms).

## Slices

- **A — decode-in-place + O(1) segment lookup — ✅ DONE.**
  `snapshot_reader::read_segment_into(seg, dst, cap)` primitive over a
  shared `decode_payload` core (both read_segment overloads ride it —
  one implementation); `unit_segment` sizes the typed destination
  vectors once from the directory's raw sizes and decodes directly into
  them (staging buffers deleted: one memset instead of two, no second
  copy, no staging mallocs); `_dir_index` built at `open()` so `find()`
  is O(1). **Measured: `<string>` used 185→143ms (−23%), unused
  154→130ms, C-path 31→29ms** — unit loads 57→34ms, register 32→23ms,
  materialize 25→21ms (the indexed find serves the restore paths too).
  Unit suite green (9019 assertions incl. test_madcdis_snapshot).
- **A2 — no-init decode destinations (post-A/D profile: memset still
  12.9%).** `madc::dis::default_init_allocator` + `decode_vector<T>` /
  `decode_bytes` (include/madcdis/pod_alloc.h): vector::resize on a
  decode destination default-initializes (no zero-fill) — every element
  is written by the decoder right after. Swapped: cir_frozen_blob
  records/children, unit connectors/positions, columnar `rb` +
  `_record_planes`, the thread-local BYTEPLANE scratch, read_unit_seg +
  its four aux readers, pool/arena block buffers, template
  tokens/payload staging, globals/extern/mir-cache/ledger decode
  locals. snapshot_reader gained decode_bytes overloads of
  read_segment / read_segment_transformed (same decode_payload core).
  Writer side untouched (allocator-transparent push_back).
- **B — lazy template payloads — ✅ DONE (B1+B2+B3+gate; measured
  numbers in the LANDED block below).** Registration keeps the
  template KEY surface eager (lookup must see the name), but
  `CirRestoredTemplate` payload/token-run decode defers to first
  instantiation request of THAT key. Kills most of the 57MB decode +
  deserialize_tokens + a large slice of materialize for programs that
  use few of a header's templates (all of them, for the unused case).
  Risk: instantiation-path correctness; the forest gates
  (selfexe/bind/emitpack/sidecar/crosstu/library/ledger/config) +
  full battery are the net.

  **LANDED 2026-08-08: B1 @4bd24886 · B2a @96744056 · B2b+B3 @ef894e26 ·
  gate @e4bcc107** — as-built deltas from this design are recorded in the
  "As built" bullet at the end of this section.

  **Measured (post-B2/B3, best-of-7, packed binary):** `<string>` used
  134→**124ms**, unused 115→**109ms**, C-path 26ms (unchanged), bare 1ms.
  Cumulative vs baseline: used 185→124 (−33%), unused 154→109 (−29%),
  C-path gap 6×→4.8×. String-probe phase detail: forest restore 45ms
  (materialize 20 + register 21, was 59 = mat 27 + reg 29 pre-B2), class
  patterns "0 materialized / 5 deferred" (was ~600 deferred eagerly —
  only THAWED defs count now), unit loads 33ms + zstd decode 43ms/76MB
  unchanged (that cost is unit node-records — slice E territory, not B).
  Batch gate green: fulltest 1002/0 incl. the new thaw-choke gate,
  libcxxjit 998/0/13skip, RELEASE+PACKED suite 1002/0/9skip on the
  lazy-thaw binary (packed added to this batch gate deliberately — B2
  rewires the packed lane's restore semantics).

  **Settled design (2026-08-08, post-D recon):**
  - *B1 — forest side (self-contained, behavior-preserving) — CODED,
    validation queued behind the D+A2 batch gate:* the materialize
    template walk goes THIN — identity only (key/name/ns/extra/owner/
    flags/pattern_reason from the record + pool; NO
    `ensure_template_payload()` call). `CirRestoredTemplate` carries the
    raw record offsets (rec_param_begin/count, rec_run_begin,
    rec_spec_count, rec_pattern_begin/words) + hydrated/hydrate_failed;
    `CirFrozenForest::hydrate_restored_template(rt)` (memoized, under
    the forest-work clock) decodes params/runs/pattern on first call.
    forest_restore_decls hydrates EAGERLY per surviving record for now
    (identical behavior; payload-broken records drop at hydrate exactly
    like the old walk's `continue`).
  - *B2 — parser side:* the restore loop registers STUB *Defs (identity
    fields + `frozen_src` = {&rt, &forest}; no restore_run). Thaw
    happens through Program-level wrapper accessors
    (`thawed_template_entry(name_id)` etc.) that hydrate an ENTRY's
    stubs in place on first content read (memoized hydration is
    logically const — const_cast inside the owner helper only).
    Read-site census (must ALL convert): template_map find_readonly ×7
    + for_each 62785; partial_spec_map ×5 + for_each; template_alias_map
    ×3 + for_each; fn_template_map/decl_map for_each 52314 + find 52509
    + 54369-54401. Existence reads (count / find!=end with no deref)
    stay direct — stubs ARE entries, existence is already correct.
    Scope order: B2a class/partial/alias lanes, B2b fn lanes; VAR/
    CONCEPT stay eager (tiny), OOL/MEMBER re-judged from the post-B2
    profile. TRAPS (recon 2026-08-08): (1) intern_keyed_map `_vals` is
    a dense vector that REALLOCATES past reserve() — never hold *Def
    POINTERS across inserts; pending lists store (map, key) or the
    (rt*, forest*) pair, and `frozen_src` must point into
    `_restored_templates` (stable after materialize), never at map
    values. (2) the freeze-writer for_each at 62785-87 exports
    EVERYTHING → thaw-all there (pack path, rare); the free-operator
    for_each at 52314 thaws only suffix-MATCHING keys inside the
    callback. (3) `restore_run` hoists from the loop lambda to a
    Program method so the recapture flush and entry thaw share it.
    GATE: a check-*.sh in fulltest denying direct content reads on the
    four maps outside the thaw-owner region (decays otherwise).
  - *B3 — deferred recapture:* `recapture_free_overload_surfaces` needs
    fd.decl at restore (free_operator_overloads / manipulator /
    free_function_overloads signature tables consulted during
    expression parse). Defer as a pending list of (rt*, forest*) pairs
    flushed ONE-SHOT by `ensure_free_overload_surfaces()` at the first
    consult of those tables (~4 sites, parser.cpp 16923/17172/17222 +
    free_function reads). The flush hydrates each rt and deserializes
    the decl run itself (independent of the fd stub's own thaw — a
    template that both captures and instantiates deserializes twice;
    acceptable). Unused TU: never flushes.
  - *Kept eager in B (v1):* VAR/CONCEPT (tiny populations), OUTOFLINE +
    MEMBER-template staging (big runs but entangled with the
    placeholder stamp flush — extend only if the post-B profile still
    shows them), v23 param-default parseExpression re-runs (separate
    eager cost, candidate for its own slice).
  - *As built (deltas from the design above):* (1) thaw landed PER-DEF
    (`thaw_template_def` / `thaw_alias_def` / `thaw_fn_def`, memoized
    via `frozen_src` NULLed up front), not per-entry — selection reads
    only identity, so `find_template` / `find_template_alias` were
    split into `*_raw` cores with thawing wrappers at the egress; an
    existence probe through them thaws only the ONE selected def.
    (2) `register_template`'s default-merge arms thaw the matched prior
    variant (both arms read/write its typeparam_defaults — a later thaw
    would clobber accumulated defaults). (3) B3 flush sites are SIX,
    not 4: the census found three more consults in the CIR builder
    (std_free_operator_instantiation, try_free_operator_call,
    std_free_function_instantiation). (4) the flush keeps a CURSOR
    (no drain) snapshotted in the journal State beside the
    overload-table sizes; rollback rewinds it so truncated surfaces
    re-derive — no lost captures, no duplicates. (5) thaw bypasses
    intern_keyed_map's transaction save (logically const): a rollback
    restoring a pre-thaw stub copy re-thaws at the next read. (6) the
    gate is marker-based (`/* thaw-owner */` / `/* identity-read */`
    on the matching line, `\b` guards exclude the eager
    var_template_map); negative control verified. (7) unit-test
    contract: direct map reads see stubs — the v20 case asserts empty
    pre-thaw + hydrated post-thaw; `_class_pattern_restore_deferred`
    now counts THAWED defs (the `>` materialized assertion became
    `>=`).
- **C — lazy registration.** register = 32ms flat, all map injection
  (`findVariable`/intern/Rb_tree ~7-8% + memcmp share). The decl-index
  sweep is 2-3ms — resolve through the frozen index on lookup misses
  instead of eagerly injecting 139 units' decls. Biggest architectural
  change; do after A+B re-measure.
- **D — stats attribution — ✅ DONE (@07377df7).** ForestWorkFrame
  (cir_freeze.h): ONE depth-guarded clock, frames at probe/open, bind
  walk, decl restore, unit loads, materialize, template payload,
  extern-index build; Program clock installed into the bound forest.
  Sampled at tokenize/parse boundaries + inside the timed CIR window
  (`_cir_forest_seconds`). lex/parse/cir now report NET with a
  "forest in phases" carve line. **Post-D honest string-probe read:
  lex-real 21ms, cir-real 26ms, forest = lex 69 + parse 0 + cir 43
  (map+open 9 + bind 1 + restore 59 [declidx 3 + mat 27 + register 29]
  + unit loads 42).** Kind-breakdown sums equal the phase carves —
  depth guard verified. Also proves: restore runs entirely under
  tokenize; on-demand unit loads entirely under cir build.

Floor estimate: C-path parity is not reachable (C++ genuinely restores
class/template surfaces C doesn't have), but unused-include ≈ C-path
cost and used-`<string>` well under 100ms look attainable from the
profile shares.

## The USE-side wall (owner recon request, 2026-08-08 post-B)

Owner observation: testsubscript.mad costs ~4× the string probe — "this
doesn't add up." Confirmed, and the forest is NOT the reason:

- testsubscript (iostream+vector+map+string, 4 container types used):
  **total 395–450ms**; forest ≈ 90–110ms of it (198 units bound, 78MB
  decoded — barely above the string probe's 76MB). The rest is USE-side:
  **instantiate 186ms (6,274 calls) + cir build 180ms (≈128ms net of
  forest)**. Callgrind: 2.4B Ir vs the string probe's 738M (3.3×).
- **Pathology by our own baseline (gcc-parity rule):** g++ -O0 compiles
  the identical source warm in ~325ms INCLUDING parsing ~100k header
  lines from text; madc with every header pre-parsed takes ~425ms. The
  instantiation lane eats the entire forest advantage.
- **Family 1 — pattern-lane refusals: 280 of ~500 class instantiations
  re-parse live** (`class instantiate: 220 pattern / 280 parse / 642
  cache / 67 opaque`). The ranked why-list is dominated by the TINIEST
  classes — metaprogramming traits: 51× `std::__and_`
  [pattern-parse-error], 20× `std::enable_if`
  [dependent-value-expression], 16× `std::__or_` [pattern-parse-error],
  10× `__is_nothrow_swappable_impl`, 10× `integral_constant`
  [pattern-parse-error], 9× `_PCC`/`__not_`/`pair` … The coarse gates:
  `basic_class_pattern_eligibility` (parser.cpp ~5768) refuses ANY
  definition with `has_non_type_params || !token_subst.empty()`
  (→ dependent-value-expression) and any `pack_subst`
  (→ unsupported-decl-kind); variadic trait bodies also fail capture
  (→ pattern-parse-error). libstdc++ internals are saturated with
  non-type params and packs, so container use falls off the pattern
  lane wholesale.
- **Family 2 — per-call constant factor** (callgrind, diffuse): ~17%
  malloc/free churn, ~8.8% string compares + hashing (string-keyed
  instantiation keys/spellings — enum-over-strings territory), 3.7%
  `dynamic_cast` RTTI probing, ~2.8% findVariable walks — × 6,274
  instantiate calls.
- **Burn-down order (proposed, next leg — no owner gate needed):**
  (1) pattern-lane NON-TYPE params — substitute the non-type binding at
  serve time; kills the dominant why-class; (2) variadic/pack patterns
  OR a Tier-1 sema fold for the trait family (enable_if / __and_ /
  __or_ / __not_ / integral_constant are semantic primitives; precedent:
  eval_void_t_detection_slot, __enable_if_t in
  eval_substituted_slot_type — design so it stays shape-keyed, not
  name-keyed, per design-principles #7); (3) instantiation-key
  interning + dynamic_cast reduction on the instantiate hot path.
  Include-side slices C/E drop BELOW this in priority — the use side is
  the bigger prize, exactly as the owner suspected.

## Facts for re-measurement

- Probes: `tmp/lexprobe_bare.mad`, `tmp/lexprobe_cstring.mad`,
  `tmp/lexprobe_nouse.mad`, `tmp/lexprobe_string.mad` (container).
- Unstripped -O2 packed twin: relink `make -C src MODE=release
  ../bin/madc-release` (skip strip), `bash scripts/forest_pack.sh
  bin/madc-release`, copy aside, then `make -C src release` to restore
  the canonical stripped artifact. Callgrind outputs at
  `tmp/cg_nouse.out` / `tmp/cg_string.out`.
- Branch: `feature/forest-string-latency-claude` off develop v0.72.0.
- QUEUED BEHIND THIS TASK: slice 2 of the channel arc (approved):
  standing API rule in cpp-first-api.md + gate ("madc-specific publics
  never REQUIRE std::string; value/const char* paths mandatory,
  std::string kept as convenience"), value overloads on madc::channel,
  eval-family value completions follow-up.
