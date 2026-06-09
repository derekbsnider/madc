# madc Front-End Representation Refactor + Optimization — FULL DESIGN

**Status:** DESIGN / PLAN (2026-06-09). Major refactor + optimization of madc's whole
lexer → parser → `cir_node` AST → c2mir/MIR pipeline. **Later-stage:** correctness/coverage
of the real libstdc++ headers comes first; this is the optimization+architecture pass that
follows. Supersedes the B3–B6 (pre-LEX package) framing of the prior header-partition plan
(that plan's correctness half — real-header consumption, shim retirement — still precedes this)
and subsumes/extends two design docs:
- `docs/plans/2026-06-09-embedded-header-forest-design.md` (the forest/modules half)
- `docs/plans/2026-06-09-lazy-member-body-instantiation-plan.md` (lazy [temp.inst], LANDED)

Must honor the set-in-stone invariants: MC11-IR (`cir_node` derives from c2mir `node_t`, is BOTH
high-level + lowered) and ADR-0001 (c2mir is the sole backend; direct-MIR is a scalpel).

---

## 0. CURRENT MINDSET (the framing, before the detailed plan)

After working the whole thread end-to-end, the synthesized understanding I'm planning from:

**(1) Not five projects — one pipeline, one through-line.** `source → lex → tokens → parse →
cir_node AST → c2mir → MIR`. "(z)PCH / modules" is *"serialize a pipeline stage's output and
reload it instead of recomputing."* Serialize after lexing → token-buffer PCH (today's `.madh`,
Phase 1). Serialize after parsing → the AST **forest / modules** (the prize: skip re-*parsing*
the stdlib).

**(2) Load-bearing law: representation choice = serialization design.** `deque<TokenBase*>` /
pointer-graph `cir_node` cannot mmap (reload = rebuild pointers — why today's `.madh` is limited).
Flat tokens + 32-bit indices / `cir_node` with handle-indirection are mmap-able, position-
independent, zero-fixup, lazy-materialize. **You cannot bolt fast PCH/modules onto pointer-graph
representations later — the representation IS the PCH design.** So the refactor spans the whole
front end even though it executes in stages.

**(3) `uid` is the spine.** c2mir's `struct node` already has `unsigned uid` and keeps its *own*
cold data (positions) out-of-line in a side-array keyed by `uid` (`node_positions`, c2mir.c:643).
The index/SoA pattern we derived is *already c2mir's design*; the fork already extracted
`c2mir_node.h` so `cir_node` extends `struct node` ABI-compatibly. So: **use `uid` as the universal
handle; madc's baggage (tokens, parse-subtree, forest-refs, provenance) = additional `uid`-keyed
side-arrays — the cleanest "c2mir-blind superset"** (c2mir walks the `ops` DLIST + reads
`code`/`uid`/`value`; never touches madc's side-arrays). Live: share `uid`. Serialized: `uid` is a
transient counter → per-module *local* id + remap on load (Clang local→global), never a cross-run
key. Explicit pre/post-check ownership (madc owns pre-check uids + provenance; c2mir may extend
during check — the "cache pre-check only" rule).

**(4) Flatten the token to kill the pointer-chase, not to shrink it.** PoC (`/tmp/tokbench`, 600k
tokens, –O2, full ~Token payload): `deque<TokenBase*>` vs flat `vector<Rec>+cursor` = **3.6–4.5×
scan/lookahead, 2.3–2.6× random/backtrack.** Win = killing pointer-deref-to-scattered-object +
vtable + deque chunking, *not* shrinking (same-size flat record already won ~3.6×). 32-bit indices
add ~10–15% speed but a 2.6× memory cut (104B→40B) → index encoding's payoff is forest size, not
speed. CAVEAT: this is the *ceiling* of the representation lever (pure token access); real
end-to-end gain is gated by % of compile spent in token access (Step-0 profiling).

**(5) The value representation is already a latent correctness bug, and pre-decides the design.**
`TokenInt` stores its literal in 64-bit `_token`; `__int128` is aliased to `ddINT64`/`ddUINT64`
(lexer.cpp:3912/3915) → `__int128` is a 64-bit fake, >64-bit literals truncate at lex. c2mir's node
value union is also 64-bit-capped. For C23/C++23 (`__int128`, `_BitInt(N)`) the value must be
**arbitrary-precision, out-of-line, handle-referenced** (Clang `APInt` / GCC `wide_int`): separate
`kind` (small) from `value` (`uint32` index into a value pool, inline fast-path for ≤64-bit). Real
correctness item, independent of perf, that everything builds on.

**(6) System-immutable / project-volatile backbone; the seam exists.** `is_system_header_path()`
already gates the DCE + lazy instantiation. System forest: built once (toolchain+std+config-hashed),
frozen, embedded read-only (`.a`/`.so`, ICU dual delivery), refs one-directional (project→forest).
Project code: volatile, content-hash-cached. salsa durability tiers = this split verbatim (adopt the
version-vector + min-durability scheme if/when incremental).

**(7) Materialize-on-resolve = Roslyn green/red, generalized with an env.** Forest = green (shared,
read-only); program-header tree = red (materialized on traversal). Delta vs Roslyn: key the
materialized layer by `(forest-node, substitution-env)` and **memoize** it (Roslyn discards red),
justified by instantiation cost. Canonicalize env keys (de Bruijn / α-equivalence) or hash-consing
→ zero sharing. The LANDED lazy member-body instantiation is this pattern one notch shallower
(materialize a body on ODR-use); the forest pushes it to "materialize-from-disk on resolve."

**(8) CIR = HIR, MIR = LIR — as vocabulary + "where opts live", NOT a mandate to own lowering.**
Semantic/type opts (devirt, copy elision, template specialization, lazy/DCE, polyglot lowering) on
CIR before lowering; register/machine opts on MIR (c2mir-generated). ADR-0001 stands: c2mir owns
HIR→LIR.

**(9) The dynamic/speculative layer: real, OPTIONAL, hook-based, fenced by cost.** Polyglot
north-star (carry Ruby etc. in `cir_node`) makes dynamic-type management first-class. Reconciliation:
**one `cir_node`, two readout modes.** No-hook → complete C11 inline-cache lowering (portable,
invariant-safe, monomorphic-fast — transpiler path). Hook-enabled (direct-c2mir/JIT only) → c2mir
calls back to madc for richer guard/side-exit detail. THREE hook flavors, very different cost:
(a) runtime IC-miss callback ≈ a plain C call (no special hook; portable path gets it too);
(b) **compile-time consultation hook** — c2mir asks madc for profile-driven specialization (real
near-term hook, JIT/REPL-only, needs a profiling loop); (c) true OSR/deopt side-exits — needs
**MIR-level OSR primitives** (Tier-3 raise MIR, research-grade; a callback can't conjure the exit).
DISCIPLINE: hooks ACCELERATE, never gate correctness (C11 lowering always complete hook-free).
`uid`-into-MIR is the substrate for ALL of it (debug info MIR lacks + hook node-identity + deopt
state maps).

**(10) Honest scoping.** Near-term achievable spine = (4)+(5)+(3)+(6)+(7) + the C11/no-hook dynamic
lowering. Fenced research-grade = OSR/deopt (MIR primitives) + full polyglot-dynamic execution.
Build foundations so the fenced track stays *reachable*; don't build it now; don't let "carry Ruby
+ guards" slide from C11-lowering into runtime-deopt (that re-opens the C11-AST invariant + the
c2mir-backend ADR).

---

## 1. Context — why this refactor

madc's front end is fast enough to *work* but its representations cap both speed and the
north-star (C23/C++23, polyglot, embedded scripting):
- The live token stream is `std::deque<TokenBase*>` (madc.h:1256) — pointers to scattered,
  heap-allocated, polymorphic token objects. A standalone PoC (`/tmp/tokbench`, 600k tokens, −O2,
  full ~Token payload) showed a flat value-record buffer is **3.6–4.5× faster** on scan/lookahead,
  **2.3–2.6×** on random/backtrack. The win is killing the pointer-chase + vtable + deque chunking.
- Literal values live in a 64-bit `_token`; `__int128` is **aliased to `ddINT64`/`ddUINT64`**
  (lexer.cpp:3912/3915, pch.cpp:263/266) and `_BitInt` is **absent** (0 hits) — so >64-bit literals
  truncate at lex time. A latent correctness bug for the C23/C++23 goal.
- PCH is a **token-stream** format (`.madh`); reloading the stdlib still re-*parses* it. There is no
  pre-parsed AST forest, and `compiler_hash` is a **static string** (pch.cpp:630), not a real
  context hash — so it can't safely key per-config caches.
- `cir_node` carries madc-only fields *on the node* and references children by pointer DLIST, so the
  tree can't mmap/serialize cheaply — yet c2mir already keeps its *own* cold data (positions)
  out-of-line in a `uid`-keyed side-array (`c2mir_next_uid`/`node_positions`, c2mir.c:18969/642), and
  the fork already extracted `c2mir_node.h` so `cir_node` extends `struct node` ABI-compatibly.

Intended outcome: a front end whose representations are **flat, index/handle-based, and
serialization-ready by construction**, so (1) lexing/parsing is materially faster, (2) wide
integers are correct, (3) the stdlib can be pre-parsed once into an embedded, demand-paged,
materialize-on-resolve **forest** (skip re-parsing), and (4) the `uid` spine carries provenance into
MIR for debug info + the optional speculative/polyglot hook layer — all without violating MC11-IR
or ADR-0001.

## 2. Architecture spine + cross-cutting foundations

The spine is **`uid`-keyed, flat, handle-indirected representation, designed for serialization.**
Cross-cutting foundations every phase builds on:
- **`uid` as the universal handle.** Reuse c2mir's `curr_uid`/`c2mir_next_uid` (c2mir.c:18969 —
  already grows a uid-keyed VARR). madc's metadata becomes **`uid`-keyed side-arrays** mirroring
  `node_positions`. Side-arrays must tolerate **sparse** uids (c2mir creates its own nodes during
  `do_context`). Single assignment chokepoint: `CirBuilder::make` (cir_builder.cpp:63).
- **Value/intern pools** (the arbitrary-precision fix): a literal/value pool (APInt/wide_int-style,
  width+limbs) referenced by `uint32` handle, with an inline fast-path for ≤64-bit; string + file
  interning. Shared by tokens and AST.
- **Hash-consing / canonical identity** for dedup + O(1) equality; the basis for the forest's
  `(forest-node, env)` memo. Canonicalize env keys (de Bruijn / α-equiv) or sharing → 0.
- **Context hash + content-hash validity** — replace the static `compiler_hash` with a real hash of
  `{toolchain, --std, layout-affecting -D/target, search paths}`; keep `source_hash` per file. The
  **system-immutable / project-volatile** split keys off the existing `is_system_header_path`
  (lexer.cpp:1619); salsa durability-tiers if/when incremental.
- **Serialize the PRE-check tree.** `do_context` mutates the tree (writes `attr`); post-check is
  single-use. The dump infra already distinguishes (`--dump-cir` pre vs `--dump-cir-checked` post).

## 3. Phased roadmap

**Critical path to the headline win (stop re-parsing the stdlib): P0 → P3 → P4 → P5.**
P1 (scan-buffer flatten) is an independent speed win. P2 (polymorphism collapse) and P6+ are fenced.

| Phase | Goal | What (anchors) | Reuse | Risk | Gate |
|---|---|---|---|---|---|
| **P0 — Value/intern pools (+ correctness)** | Arbitrary-precision literals (`__int128`/`_BitInt` correct); the handle scheme | Out-of-line value pool (APInt-style), `kind` split from `value`-handle; real `__int128`/`_BitInt` `DataDef`s (replace ddINT64 alias, lexer.cpp:3912); widen the int64-capped consumers: `ival()`/`get()` (26+45 sites), `parse_constant_*` rungs, `ioperate/foperate` | string/`source_text` interning patterns; `resolve_int_suffix_type` (lexer.cpp ~3030) | Transitive (constexpr/fold chain widening) | fulltest; torture-ALONE; a wide-literal correctness test vs g++ | 
| **P1 — Flat token scan buffer** | The 3.6–4.5× lex/parse-read win | Replace `deque<TokenBase*> tokens` + cursor (madc.h:1256/1521-1591) with a flat value-record buffer + **index cursor** (backtrack = index rewind) + a **token source-stack** for the ~95 injection sites (82 `pushToken` + 8 `pushCompound` + 5 `injected_tokens`) | **`Source::_pushback_frames` (madc.h:736-825) is the source-stack template**; does **NOT** touch cir_builder (it walks the retained AST, not the stream) | Medium — the injection-site rewrite; correctness-neutral if API preserved | fulltest; torture-ALONE; **Step-0 profile first** (is token access hot post-findVariable-fix?) |
| **P2 — Polymorphism collapse (FENCED, optional)** | Flatten retained AST tokens → tag+union | `TokenBase` virtual hierarchy → tag + `switch`; the blast radius: **1577 `->id()`, 388 `->type()`, 574 `dynamic_cast`, 148 `->left/right/operand`, 119 classes** | tag from the existing `TokenID`/`TokenType` enums | **HIGH** — codebase-wide | only if profiling proves the AST (not the scan buffer) is the bottleneck; **default = fence** |
| **P3 — `uid` side-arrays + serialization-ready cir_node** | cir_node = `struct node` + uid-keyed side-tables; c2mir-blind by construction; uid→MIR | Migrate the 6 madc-only `cir_node` fields (`origin`,`datadef`,`typedef_name`,`error_msg`,`src_lang`,`synth_from_origin`, cir_node.h:55-65) into `uid`-keyed side-arrays; positions already a view over `origin` (cir_builder.cpp:48-50). Add uid→MIR carry (net-new; MIR has no debug slot — confirmed) | `c2mir_next_uid`/`node_positions` template; single chokepoint `CirBuilder::make`; ~30 `CIR_NODE(n)->` sites + 2 walkers (`cir_dump_node`, `cir_walk_errors`) | Medium — sparse-uid tolerance; `CIR_NODE` only valid for cir_node-allocated nodes (c2mir-created nodes are plain `struct node`) | fulltest; torture-ALONE; `--emit=c11` unchanged (already c2mir-blind); canaries |
| **P4 — AST forest serialization (Phase-2 PCH)** | Serialize/mmap the pre-parsed cir_node graph; skip re-parsing | Extend `MadhHeader` → forest format (version + separate Type/Decl ID tables + **per-unit zstd frames + ID→offset TOC + shared trained dict** + real context-hash); uid-based child lists (via `c2mir_node_op`, c2mir.c:18989); mmap → demand-page → decompress-frame-to-arena | `serialize/compress/write_madh/read_madh`+`compiler_hash` reject discipline (pch.cpp); `push_precompiled_header_tokens` name-rebind pattern (lexer.cpp:1775); the `--emit-pch`→gen-script→`xxd -i`→embed bootstrap | High — graph (not token) serialization; compression⟂zero-copy → two layers | fulltest; torture-ALONE; SMAUG; round-trip identity (load==reparse) |
| **P5 — Static/dynamic forest + modules** | Per-file immutable forest, materialize-on-resolve, durability split | Per-file units; system-immutable (toolchain+std+config-hashed, embedded `.a`/`.so` + ICU sidecar) vs project-volatile (content-hash); extend `materialize_and_lower` (cir_builder.cpp:9308) from **reparse-tokens → copy-cir_node-subtree+substitute-env**; `(forest-node,env)` hash-cons memo replacing the `registered_mangled` string key | `is_system_header_path`; `find_precompiled_header` API shape → `find_forest_unit`; the landed lazy machinery (`deferred_lazy_bodies`/`materialize_and_lower`/`referenced_funcs`); `datatype_map`/`struct_map`/`is_complete` as the per-node materialization-state | High — the env hash-cons + cross-unit local-id remap | fulltest; torture-ALONE; SMAUG; real-`<iostream>`/`<string>` run == g++; macro/dM parity |
| **P6 — Optional c2mir hook seam (FENCED)** | HIR↔LIR connection: debug info now; speculation later | uid→MIR (debug/diagnostics) — buildable now. Compile-time **consultation hook** (c2mir callback into madc, JIT/REPL-only, needs a profiling loop). C11 inline-cache lowering is the no-hook baseline | the fork is co-owned (madc owns MIR); `cir_import_resolver` (madc_cir.cpp:74) + eager RTLD_GLOBAL dlopen for the external-link leg | High; **discipline: hooks accelerate, never gate correctness** | C11 path runs hook-free; torture-ALONE |

## 4. c2mir node_t co-design (the spine, detail)

- **uid ownership:** c2mir owns `curr_uid`; madc pulls via `c2mir_next_uid` (which already extends
  `node_positions`). Keep that contract; add parallel madc-owned `uid`-keyed VARRs at `CirBuilder::make`.
- **Side-arrays = the c2mir-blind superset, structurally.** c2mir walks `ops` + reads `code`/`uid`/
  `u.value`; it never touches `node_positions` or any madc side-array. So moving madc's 6 fields into
  side-arrays makes blindness *automatic* (confirmed: `cir_emit_c.cpp` already reads zero madc fields).
  Materialization (forest→program) = "promote a forest-ref into a `node_t`-visible `ops` child."
- **Value representation in the node:** `u` union is 64-bit-capped → wide literals reference the P0
  value pool (out-of-line). This is the node-level half of the `__int128`/`_BitInt` fix.
- **uid → MIR:** net-new carry (MIR `MIR_insn` has no position field; no DWARF). Add a uid side-table
  in MIR gen (fork) so a runtime fault/diagnostic maps MIR loc → uid → `origin` → file/line/col. This
  single mechanism serves diagnostics, debug info, the compile-time hook's node identity, and any
  future deopt state map.
- **Serialize pre-check** (attr is meaningless until `do_context`); per-TU uid → per-module local id +
  remap on load (uids reset per `c2m_ctx`; c2mir-injected check-time nodes have no madc side-data).

## 5. Static/dynamic forest + modules

Subsumes `docs/plans/2026-06-09-embedded-header-forest-design.md` (read it for the full prior-art
synthesis — Clang lazy-AST, Roslyn green/red, salsa durability, the verdict table; this plan does not
restate it). Net: per-file immutable **green** forest + materialized **red** program-header tree;
materialize-on-resolve keyed by `(forest-node, env)` with memo; system/project = `is_system_header_path`;
external-link leg satisfied by the existing eager RTLD_GLOBAL dlopen (NOTE the correction: live
resolver is `cir_import_resolver`, **no dlopen-on-miss** — task #7's "R2 auto-load" framing is wrong;
either keep eager dlopen or add an on-miss fallback). Embed via `xxd -i` (extant) + optional ICU-style
sidecar. Reject-and-reparse on context-hash mismatch (never silently use a layout-mismatched forest).

## 6. Lazy-instantiation integration (extends the landed work)

The landed lazy member-body instantiation (`docs/plans/2026-06-09-lazy-member-body-instantiation-plan.md`,
commits 2173ae0/11ac1bc/aef0366) IS the materialize-on-resolve seed: `deferred_lazy_bodies` (keyed by
emit symbol = `var.name`), the `materialize_and_lower` fixpoint, `referenced_funcs` as the ODR-use
trigger, the `is_extern_template_instantiated` C1/D1 external-link bind, and the
`instantiating_canonical_spelling` save/restore (= the proto-`env`). P5 generalizes it: the deferred
unit becomes a forest `cir_node` subtree (copy+substitute, **no reparse** — the headline gap), and the
string key becomes the `(forest-node, env)` hash-cons. Task #25's remaining richer-string work
(member-template ctor not entering `cdd->ctors`; `pointer_traits<char*>`) is **member-TYPE/layout**,
independent of this serialization work — do not couple them.

## 7. Optional c2mir hooks / HIR-LIR / speculation (3 flavors, fenced)

CIR=HIR / MIR=LIR is **vocabulary + "where opts live"** (semantic opts on CIR; machine opts on MIR),
NOT a mandate to own lowering (ADR-0001). The dynamic/speculative layer is **one cir_node, two readout
modes**: no-hook → complete C11 inline-cache lowering (portable, invariant-safe); hook-enabled
(direct-c2mir/JIT) → c2mir calls back to madc for richer guard/side-exit detail. Three flavors by cost:
(a) runtime IC-miss callback ≈ a C call (no special hook, portable gets it too); (b) **compile-time
consultation hook** (the real near-term one, JIT/REPL-only, needs a profiling loop); (c) true OSR/deopt
side-exits → **MIR-level OSR primitives** (Tier-3 raise MIR, research-grade). Discipline: **hooks
accelerate, never gate correctness.** `uid`→MIR (P3/P6) is the substrate for all three.

## 8. Fenced future (research-grade; build foundations, not these)

- **OSR/deopt** (MIR OSR primitives + a baseline tier + state maps) — needed only for top-tier
  speculative execution of dynamic ops; gated on a real perf need.
- **Polyglot dynamic-language execution** (carry Ruby etc. with native dynamic ops): the *transpiler*
  path (lower dynamic ops to C11 inline caches) is invariant-faithful and achievable; the *speculative
  executor* (runtime deopt) is the fenced extension. Do not let "carry Ruby + guards" slide from
  C11-lowering into runtime-deopt — that re-opens the C11-AST invariant + the c2mir-backend ADR.
- **P2 polymorphism collapse** is fenced-by-default (above): the scan-buffer flatten (P1) gets the win
  without it.

## 9. Sequencing, dependencies, risks

- **Order:** P0 (foundations/correctness) → then P1 (independent speed) ∥ P3 (uid side-arrays) → P4
  (serialize) → P5 (forest). P6 after P3. P2 fenced. **All of this is AFTER the current correctness
  work** (real-header coverage, shim retirement, task #25) — it's the optimization/architecture pass.
- **Top risks:** (1) compression ⟂ zero-copy → two-layer (frames + decompress-to-arena); (2) memoize
  on canonical/global id, not use-site (Clang's hardest bug class); (3) canonicalize env keys or
  hash-consing → 0 sharing; (4) pin the context-hash, reject-and-reparse on mismatch; (5) sparse-uid /
  c2mir-created-node tolerance; (6) the value-pool widening is transitive through constexpr/fold; (7)
  P2's 1577+574+148-site blast radius — keep it fenced.
- **Standing gates every commit:** `make -C src fulltest` (known reds only), **gcc.c-torture run
  ALONE** (1566/31/57/1 unchanged — these are all C++-gated/representation changes, so the C path must
  be invariant), SMAUG C89 soak, and the real-`<iostream>`/`<string>` run == g++. g++/clang are the
  oracle for values.

## 10. Verification

- **Per phase:** the gate column in §3, plus a phase-specific oracle — P0: a `__int128`/`_BitInt`
  literal+arithmetic test matching g++; P1: re-run the `/tmp/tokbench` PoC pattern + Step-0 real-compile
  profile (token-access % before/after); P3: `--emit=c11` byte-identical before/after (proves blindness);
  `--dump-cir` provenance intact; P4: forest round-trip — a header loaded-from-forest produces a
  `cir_node` tree structurally identical to re-parsing it; P5: real `<iostream>`+`<fstream>`+`<string>`
  programs compile, link (eager dlopen), and produce g++-matching output, with the forest mmap'd.
- **Acceptance oracle (stand up before P4):** `madc -dM` macro parity vs `gcc -dM`; C-smoke + C++-smoke
  header sets; end-to-end RUN parity; `check_partition_drift`/context-hash green.

## 11. Relationship to existing plans/docs

- This plan **supersedes** the header-partition campaign's B3–B6 (pre-LEX package) framing; that
  campaign's **correctness half** (real-header consumption, shim retirement M, R1/R3/R4) **precedes**
  this and is unchanged (state in memory `project_header_partition` + `claude_status.json` + the
  handoff docs).
- **Subsumes/extends** `docs/plans/2026-06-09-embedded-header-forest-design.md` (forest/modules — §5)
  and `docs/plans/2026-06-09-lazy-member-body-instantiation-plan.md` (lazy [temp.inst], LANDED — §6).
- Honors `.claude/rules/mc11-ir.md` (cir_node IS node_t + high-level) and `docs/adr/0001-cir-c2mir-backend.md`
  (c2mir sole backend; direct-MIR scalpel) and `.claude/rules/lowering-vs-raising.md` (OSR/`_BitInt` =
  Tier-2/3 raises, fenced).
- **Corrections found during code-grounding (fix in the campaign docs):** (a) live import resolver is
  `cir_import_resolver` (madc_cir.cpp:74); `madc_import_resolver` (madc_mir_backend.cpp) is dead. (b)
  There is **no dlopen-on-miss / "R2"** — external symbols resolve via eager `-l`/`#load` RTLD_GLOBAL
  dlopen; task #7's framing is inaccurate. (c) `compiler_hash` is a static string, not a context hash.
</content>
