# Embedded Header Forest — design model + 2026 prior-art research

**Status:** DESIGN BRAINSTORM (2026-06-09). This is a **later-stage optimization** —
correctness/coverage of the real libstdc++ headers comes first (real `<string>`/`<iostream>` etc.), *then* this. New file; supersedes the **pre-LEX raw-token** framing of
`clever-scribbling-dove.md` B3–B6 with **pre-PARSE (full `cir_node` AST)**. Builds on the
MC11-IR invariant (`.claude/rules/mc11-ir.md`) and the landed lazy member-body instantiation
(`docs/plans/2026-06-09-lazy-member-body-instantiation-plan.md`).

> **UPDATE 2026-06-12** — this doc remains the **prior-art / mental-model reference**; the
> *operative phasing* is `docs/plans/2026-06-09-frontend-representation-refactor.md` (forest =
> P4/P5 there). Two reconciliations since writing:
> - The Part-3 verdicts "Copy Clang: ID/offset cross-refs, separate Type tables, type-ID
>   low-bits-for-builtins" now have an **AGREED concrete madc design**:
>   `docs/plans/2026-06-12-type-table-value-abi-design.md`. Forest type references serialize
>   as uint32 typeids, and the table's **system segment `[100, 0x01000000)` IS this doc's
>   "frozen at build time" id space** — name→tree index entries resolve type identity through
>   it. The ID/handle indirection called for under "Storage / embedding" is therefore no
>   longer an open design item for *types* (tokens/nodes: refactor P1/P3).
> - Citation note: `clever-scribbling-dove.md` was since overwritten (it now mirrors the
>   refactor doc); the B3–B6 referent survives only as reconstructed in
>   `docs/plans/2026-06-08-header-partition-HANDOFF.md`. The system-header reachability DCE
>   referenced alongside lazy bodies is live on develop (`cir_builder.cpp` reachability
>   fixpoint), despite its own handoff doc predating the landing.

## Context — why

C++ headers are an enormous glut; parsing the libstdc++ closure dominates compile time. The goal is **speed**: support a **standard define-set BLAZINGLY FAST**; a non-standard config (`NDEBUG`, odd feature macros, `-m32`, `_FILE_OFFSET_BITS`, …) pays a **live-parse penalty**.
We deliberately do **not** fund covering every variation out of the box.

---

## Part 1 — The mental model

### The `cir_node` superset c2mir is blind to

The live `cir_node` tree is a **strict superset** of the `node_t` tree c2mir compiles. c2mir
sees only a *projection*; madc sees the whole forest-linked structure. Three regions:

1. **Program code AST** — the user's statements. Volatile, fully c2mir-visible.
2. **Program-header AST tree** — the subset *materialized on demand* from the forest by actual uses. c2mir-visible (the instantiated/ODR-used result).
3. **Forest reference links** — pointers into the immutable forest. **c2mir-BLIND.** Pure
   availability; emit nothing.

**Why c2mir is blind to (3):** `cir_node` derives from `node_t`; c2mir traverses via `node_t`
*child* links. Forest references hang off **madc-only fields**, so c2mir physically cannot reach them. **Materialization = "promote a forest reference into a `node_t`-visible child of the emit tree."** `translate_module` building `top_list` already *is* this projection. **The forest's size is irrelevant to c2mir — the blindness IS the optimization.**

### Baggage: availability vs use (standard C++ semantics; already how madc runs)

- `#include <iostream>` = **populate availability** (bind names → forest links; madc-only edges).
  Emits nothing. Remove it → links vanish → dangling = the invalidation signal.
- `std::cout << "x" << std::endl;` = **ODR-use** → resolve through links → materialize the needed forest subtrees into region (2) → those enter the projection → c2mir.
- A use either (a) resolves to an **external** symbol → stays a *link* (satisfied at JIT-link vs
  `libstdc++.so`: `cout`, extern-template instantiation symbols, `C1`/`D1`), or (b) needs a **madc-owned definition** → materializes + projects to c2mir. So even on the "use" side most of `<iostream>` stays a blind link.

### Instantiation = copy-on-RESOLVE (not eager clone)

Clone-then-substitute is two traversals. Instead, copy **during** resolution: an instantiation starts light = `(forest template ref, env {_CharT→char}, empty memo)`; a node materializes only when something resolves through it — the consumer's traversal *is* the copy traversal; untouched nodes are never copied. The unmaterialized "reference node" + env **is** the lazy node; resolving = expanding = materialize-and-memoize one node. Costs (the whole cost): (1) the resolver carries
the env; (2) a **memo keyed by `(forest-node, env)`** for identity/dedup (hash-consing); (3) the forest stays read-only, materialized nodes go to the program arena.

Sizing caveat: `compute_layout` walks all members → the class *shell* (member types) materializes regardless. Savings concentrate in (a) method **bodies** (only ODR-used ones — exactly the landed lazy-body work) and (b) eliminating the separate copy pass. The forest's real payoff vs today:
today instantiate = clone **tokens** + **re-parse**; forest = copy the `cir_node` subtree +
substitute, **no reparse**. Killing the reparse is the win.

### System (immutable) vs project (volatile) — the backbone

**madc already draws this line: `is_system_header_path()`** (the same gate driving the DCE + lazy instantiation). Two tiers, two policies:

- **System forest:** built once at `libmadc` build/install time, keyed by
  `compiler_hash + std + canonical-config`; never invalidated within a toolchain; embedded, read-only, shared. Freeze at build time: name→tree index, internal reference offsets, mangled names, layout. References are **one-directional (project→forest)** → a sealed self-contained DAG.
- **Project code (incl. project headers):** parsed live, or content-hash-cached per-project
  (`source_hash`, already in `MadhHeader`); volatile; not embedded.
- **Boundary case** `std::vector<MyType>` (system template + project type): the materialized instantiation is **always program-tree-owned** (volatile side), referencing the read-only forest template + the user type. Forest stays pure.

### Storage / embedding

- madc already has the framework: `include/madc_pch.h` `MadhHeader` (`source_hash` + `compiler_hash` + zstd/zlib) and its comment literally says *"Phase 1 = token stream,
  Phase 2 = AST, Phase 3 = modules."* This is Phase 2.
- Per-unit compressed + index (not one stream). `cir_node` is a **graph** → serialize via
  **ID/handle indirection** (resolve-on-touch), not pointer relocation.

---

## Part 2 — 2026 prior-art research

Three parallel research agents (Clang/modules; Roslyn red-green + query compilers; variability-aware parsing + serialization). The convergent headline: **this design is proven-good-practice; it is essentially Clang PCH/modules + Roslyn green/red trees + salsa durability tiers.** Spend the novelty budget on the few genuine deltas, not on re-deriving the loader.

### 2a. Clang PCH / C++20 modules — the closest production precedent

Clang has done "immutable forest + materialize-on-resolve" since ~2009:

- **On-disk hash table** name → decl-ID; per-`DeclContext` visible-decls table; **lazy per-decl deserialization** (an entity + its dependency closure load only when first referenced).
  *This is exactly our "materialize touched subtrees."*
- Cross-refs are **IDs/offsets** (type-ID shifted-bits trick: low bits = builtins, no table
  lookup). **Separate Type vs Decl** ID spaces (types are far more numerous/shared) — worth copying.
- **O(1) attach:** initial load is independent of file size — the real payoff of "pre-parse once."
- **mmap / zero-copy**, lazy bitstream decode. **Not general-compressed** (LLVM bitstream is compact + mmap'd; gzipping would force whole-file decompress and *kill* lazy random access).
- **Reduced BMI** (`-fmodules-reduced-bmi`, stable in Clang 23): strips non-inline bodies + GMF unreachable entities from the interface; records *contributory* hashes so an implementation change that doesn't alter the interface yields the **same BMI hash** → dependents skip rebuild.
  **= our "hand the backend only what's used"; the embedded forest should be the reduced/interface form, with bodies re-expanded at codegen.**
- **Context hash:** Clang hashes semantics-affecting flags (`-D`, search paths, dialect, target) → different context = different `.pcm` variant or rebuild. **We can mostly skip this BUT must enforce a pin:** our forest is parsed under ONE fixed context; a divergent `-D`/target (`_FORTIFY_SOURCE`, `-m32`, `_FILE_OFFSET_BITS`, `-fshort-enums`, …) changes *layout/visibility*
  — using the forest then is **wrong, not just suboptimal.** So: reject-and-reparse (or ship a small set of keyed variants) on mismatch; never silently materialize against a mismatched forest.
- **Cross-module merging / global IDs / ODR:** Clang unifies identical entities from multiple modules into one canonical redeclaration chain (global ID space, structural dedup). This is its hardest, still-buggy area (15 yr in). **We can largely SIMPLIFY it away:** a single immutable C provider ⇒ no "two modules both declare `printf`" ⇒ identity is just the precomputed global ID.
  Memoize on **canonical/global ID, not use-site.** Don't frame concrete C decls as template "substitution."

### 2b. Roslyn red-green trees — near-exact analogy (with the delta named)

- **Green = our forest** (immutable, position-independent, structurally shared/reused across all use-sites). **Red = our materialized program tree** (lazy façade built top-down on traversal; parent/position manufactured on descent; discardable). Edits rebuild only the O(log n) root path; the rest is shared by reference (= "copy = share until divergence").
- **The genuine delta:** Roslyn keys a red node by `(green, path)` and does **not** memoize/dedup red nodes (they're transient). We key by `(forest-node, **env**)` and **do** memoize the materialized layer. The **env (substitution context) is the novel generalization** — Roslyn has no substitution dimension. Memoizing the façade is justified *only* because env-keyed materialization (template instantiation) is expensive — make that the explicit rationale.

### 2c. Query-based incremental compilation (rustc, salsa, rust-analyzer)

- **Vocabulary trap (keep separate in all docs):** Roslyn red/green = *data colors* (immutable vs façade). rustc red/green = *dependency-graph validity* coloring (`try-mark-green`, early cutoff). Orthogonal; a system uses both.
- **salsa durability tiers = our system-immutable / project-volatile split, exactly.** salsa marks stdlib inputs *durable*, project files *volatile*, uses a **version vector** (one counter per tier) and **min-durability inheritance**, so a project edit bumps only the volatile counter and stdlib-derived results are skipped at validation. **Steal this verbatim** if/when we want incrementality. salsa **interned structs** = hash-consing for our `(forest-node, env)` keys.
- **Adapton / self-adjusting computation** = the theory (demand-driven memoized recompute); consume it via salsa, cite it for soundness, don't implement it.

### 2d. Variability-aware parsing (TypeChef, SuperC) — rejected, correctly

- Keeping `#if/#else` as **presence-condition** nodes (one tree, all configs) is real research (TypeChef OOPSLA'11, SuperC PLDI'12) but it is **analysis tooling, not compilation**: parsing x86 Linux took TypeChef **~85h**; SuperC is 3.4–3.8× faster but still a multi-branch research parser. The central difficulty is the **type/value ambiguity** (a token that's a typedef-name in one config, an identifier in another). Worst case exponential in dense, interacting variability (Kconfig-scale).
- **Verdict: rightly rejected.** Our variation is shallow/orthogonal (`__cplusplus` + a few
  feature macros), not Kconfig-scale. "Parse one canonical config fast + reparse non-standard" *is* the field's implicit verdict for shallow variability.

### 2e. Serialization / compression / mmap / embedding

- **Per-unit compression + explicit ID→offset TOC** (git packfile + `.idx`; ICU `.dat` TOC), *not* one stream and *not* zstd's offset-keyed seekable table.
- **zstd trained dictionary** solves the small-frame ratio loss: per-unit frames compress near whole-blob ratios while staying independently decompressable (measured: per-doc 6.8 MB → with 64 KB dict 4.9 MB vs monolithic 1.8 MB). Training is a build-time cost (fine); ship the dict embedded.
- **Compression and zero-copy mmap are mutually exclusive at one layer.** You mmap *compressed* bytes cheaply (OS demand-pages them; untouched frames never fault) but must **decompress into an arena** to read structured nodes (those become private/anonymous pages). So OS laziness is at **frame granularity** — keep frames small (frame size = the tuning knob: ratio vs fault granularity). Architecture = **two layers: compressed-at-rest frames under an ID-indexed lazy-materialization scheme.**
- **ICU is the embedding precedent:** the *same* 16-byte-aligned image ships **either** as a
  static-lib byte array **or** as an external mmap'd file, with an internal TOC. Copy its alignment discipline + dual delivery → defer embed-vs-sidecar to a packaging flag.
- **Zero-copy formats** (Cap'n Proto / FlatBuffers): position-independent relative offsets, mmap-able — good for the *materialized* arena — but an AST is a **DAG** (shared nodes), which FlatBuffers handles awkwardly (bottom-up build) and Cap'n Proto allows (backward refs). **Disable verification** (its O(n) pass defeats lazy fault-in). Resolve shared refs through an **ID table** (Clang's way), not a literal on-disk pointer graph.
- **Hash-consing / maximal sharing** (ATerm **BAF** preserves sharing *on disk*; Clang declaration merging) is the cross-unit dedup substrate. **Canonicalize env keys** (de Bruijn / α-equivalence / sorted bindings) or hash-consing silently degrades to no sharing.
- **`--gc-sections` retention gotcha:** an embedded `.rodata` blob is dropped unless the loader holds a live reference.

---

## Part 3 — Verdicts

| Concern                                                                        | Verdict                                                         | Precedent                |
| ------------------------------------------------------------------------------ | --------------------------------------------------------------- | ------------------------ |
| name→ID hash table, per-context visible-decls, lazy per-decl load, O(1) attach | **Copy Clang**                                                  | Clang PCHInternals       |
| ID/offset cross-refs; separate Type vs Decl tables; type-ID shifted-bits       | **Copy Clang**                                                  | Clang                    |
| mmap zero-copy; **don't gzip the in-place layer**                              | **Copy Clang/ICU**                                              | Clang, ICU, OS paging    |
| green/red two-layer (immutable forest + lazy materialized façade)              | **Adopt Roslyn**                                                | Roslyn red-green         |
| Reduced-BMI interface/body split + contributory-hash narrowing                 | **Adopt the idea**                                              | Clang Reduced BMI        |
| durability tiers for system-immutable vs project-volatile (if incremental)     | **Adopt salsa**                                                 | salsa / rust-analyzer    |
| hash-consing/interning for `(forest-node, env)` identity                       | **Adopt; canonicalize keys**                                    | salsa, ATerm/BAF         |
| per-unit zstd frames + shared trained dict + ID→offset TOC                     | **Adopt**                                                       | git pack, ICU, zstd      |
| `#if`-nodes-in-AST (variability-aware)                                         | **Reject** (analysis-only, heavyweight)                         | TypeChef/SuperC          |
| context hash for `-D`/target variance                                          | **Skip but PIN + reject/reparse on mismatch**                   | Clang context hash       |
| cross-module merging/ODR                                                       | **Simplify away** (single immutable C provider ⇒ ID = identity) | Clang (its hardest area) |

### The genuine deltas (our contribution, no off-the-shelf answer)

1. **Env-parameterized materialization** + **memoizing the façade** (Roslyn keys by `(green, path)`
   and discards red; we key by `(forest-node, env)` and cache — justified by instantiation cost).
2. **c2mir backing** — the lowering-to-MIR boundary and how memoized materialized nodes feed MIR is our integration surface; design, don't borrow.
3. **Single-immutable-C-provider simplification** — lets us drop the merge/context-hash machinery that causes most of Clang's modules bugs.

### Traps to avoid (called out by the research)

- Compression ⟂ zero-copy at one layer → two-layer (compressed frames + decompress-to-arena).
- **Memoize on canonical/global ID, not use-site** (or duplicate, non-`==` nodes break type identity — Clang's hardest bug class).
- **Canonicalize env keys** or hash-consing degrades to zero sharing.
- **Pin the context**; reject/reparse on layout-affecting `-D`/target mismatch.
- Keep the two "red/green" vocabularies strictly separate in all docs.

---

## Sources

Clang: PCHInternals, StandardCPlusPlusModules, Modules (context hash), LibASTImporter (clang.llvm.org/docs); C++20 Modules status (Chuanqi Xu, 2025). Roslyn red-green: Eric Lippert (2012), Osenkov "Roslyn Immutable Trees". Query/incremental: rustc-dev-guide (incremental in detail), salsa book + "Durable Incrementality" (rust-analyzer 2023), Adapton (PLDI'14). Variability: TypeChef (OOPSLA'11), SuperC (PLDI'12). Serialization: zstd (RFC 8878, dictionaries, seekable), Cap'n Proto vs FlatBuffers, ICU Data docs, ATerm/BAF, git packfiles, squashfs, hash-consing (Filliâtre & Conchon; α-equivalence hashing arXiv 2105.02856).
