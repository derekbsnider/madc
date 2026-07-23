# madc — Embedded Header AST & Frontend Design

**Status:** design
**Priorities (in order):** Fast > Robust (small/powerful) > Flexible (highly capable)
**Constraint:** compact, self-contained artifact

This document consolidates the design for madc's frontend pipeline, the `cir_node`
CIR-AST structure, and the embedded (precompiled) stdlib header format. madc is a
polyglot language built on top of C/C++, not a drop-in clang/gcc replacement; the
embedded stdlib is the default, with a live-parse escape hatch.

---

## 1. Frontend pipeline

Two linear passes, no tree-to-tree lowering:

```
source → [streaming PP] → append-only token array → [recursive-descent parser] → CIR-AST (node_t backbone)
```

- **Pull-based preprocessor.** Expands macros at the frontier into an *append-only*
  token array. No deque — the deque only existed to absorb macro push-front from
  inline expansion, and a streaming PP with an append-only sink removes that need.
- **Rewind cursor.** The parser walks the token array with a cursor; because the
  array is append-only, already-expanded tokens never move, so backtracking is a
  free cursor rewind. This covers the C-family hard cases: the lexer hack
  (typedef disambiguation), template `>>`, and the most-vexing-parse.
- **Direct handoff.** The parser builds directly on c2mir `node_t`. No second tree,
  no lowering pass before the backend consumes it.
- **FFI / third-party headers always take this live path**, independent of embedded
  mode. The live PP + parser is load-bearing regardless of `--no-embedded-headers`.

---

## 2. CIR-AST representation (`cir_node`)

`cir_node` is the lowered, typed, semantic IR that c2mir consumes — *not* a
fidelity-preserving CST. All language sugar (C++ overloads, references, default
args, namespaces; C implicit conversions) is desugared during construction so the
backend sees only explicit, typed nodes. This keeps c2mir genuinely language-blind.

- **Arena + `u32` index handles**, no internal pointers. This is the single decision
  that makes freeze/thaw, segmentation, and zero-fixup embedding all trivial.
- **Lean `node_t` backbone** — the hot, handed-off structure. Minimal, symmetric
  node-kind set; resist per-language node kinds.
- **Side-car tables keyed by node index**: C++ decoration, token ranges, source
  spans. Cold during codegen; droppable after semantic analysis. (Thermal model.)
- **Interned type table**, referenced by index. `_Complex` and `vector_size` SIMD
  types are type-table entries, *not* node flags. Vector ops stay ordinary typed
  binary/unary nodes over vector-typed operands — no special SIMD node kinds.
- **Interned string/identifier table = an arena-model hash table** (the backing
  store for `TokenRec.spelling_id` and every interned name; sibling of the type
  table). To be `mmap`-in-place zero-copy (§4/§9) it is **index-linked, not
  pointer-chained** — three contiguous blocks: (1) a **byte arena** (spelling bytes,
  append-only); (2) an **entry arena** `{ u32 byte_off, u32 len, u32 hash, u32 next }`
  where the **`id` IS the entry index** and `next` chains collisions **by index**;
  (3) a power-of-two **bucket array** of `u32` head-indices. `intern(bytes)→u32`
  dedups on insert; `hash` stored inline rejects probes before touching bytes.
  Algorithm = tinycc `TokenSym`/SMAUG `hashstr`, but index-based so it serializes
  with zero fixup. Segmented per §5: the embedded snapshot ships a **frozen** byte+
  entry+bucket segment owning an id range; the TU **appends entries above** and
  resolves misses against a TU-private bucket extension (keeps the embedded block
  zero-copy). Buckets are derived: a decompress-to-arena segment may rebuild them
  at load instead of storing them.
- **Builder discipline.** The shared builder only permits typed, semantically-valid
  construction. Each `LangDescriptor`'s lowering owns its own desugaring, so
  per-language quirks cannot leak into the backbone or the backend.

> Note: a true lossless CST was considered and rejected. It serves Flexible
> (LSP, source round-trip) at the cost of a richer node set and more memory, which
> fights Fast and Robust-small. The decorated `node_t` tree with retained token
> ranges already provides ~90% of the CST benefit, lazily.

---

## 3. On-disk embedded format

Compression and zero-copy do not have to conflict, but they cannot apply to the
*same bytes* — resolve by **segmenting** so each priority gets what it wants.

- Container = **segment directory** + N independently-compressed segments.
- Codec is **per-segment** (see §9) — fast codec (zstd/lz4), decode-speed over ratio.
- **Backbone and side-car segments are separate.** Backbone is hot (touched at
  codegen); side-cars are cold (touched only by semantic analysis / diagnostics).
- Each segment owns a **reserved logical index range**; the directory maps
  index-range → segment. Compression boundaries and index boundaries coincide.

This keeps the shipped artifact compact (compressed on disk) without forcing a
whole-file decompress to touch a single header.

---

## 4. mmap + load / expand-on-demand

mmap the **whole container**; compression composes with it cleanly. The OS pages in
compressed segment bytes lazily, and decompression happens per-segment on first
touch. Two levels of laziness: untouched segments' compressed bytes never page in,
*and* their CPU decompress never happens.

Resolution of handles depends on the segment's codec:

- **`codec=none` segments → true zero-copy.** Handles resolve *directly into the
  mmap*. No arena copy. Use this for the hot backbone.
- **Compressed segments → decompress-to-arena.** On first reference, decompress the
  mapped (compressed) bytes into an arena region sized to the segment's reserved
  index range; handles resolve into the arena. The mapping stays read-only shared;
  the arena is private. Use this for cold side-cars.

After semantic analysis, cold side-car regions can be dropped — backed by "never
decompressed in the first place" rather than "freed."

Net: backbone is zero-copy mmap-in-place (Fast); side-cars stay squeezed on disk and
frequently never materialize (compact).

---

## 5. Index space (zero fixup)

- **Segmented, append-only logical index space.** Each embedded module/header owns a
  reserved range; its handles are valid as-stored.
- On consume, the current TU **appends above** the embedded ranges. No local→global
  relocation, no merge pass.
- Interned type/identifier IDs follow the same rule: embedded snapshot owns its
  range, TU appends.
- Segment directory entry carries a **file offset** and, for `codec=none`, the
  **in-mapping base** that handles resolve against. Decompress-to-arena segments
  resolve against the arena region allocated for their reserved range.

---

## 6. Dev / prod modes (deliberately opposite priority orders)

| | Mode | Priorities | Headers | `-D` |
|---|---|---|---|---|
| **Production** | embedded (default) | Fast > Robust > Flexible | mmap'd baked stdlib AST, sealed PP env | does **not** reach embedded headers |
| **Development** | `--no-embedded-headers` | Flexible > Robust > Fast | live PP + parse over installed system headers | flows normally |

The divide **is** the contract. Need `-DNDEBUG` (or any `-D`) to reach system
headers? Install them and use `--no-embedded-headers`, and pay the parse penalty.
Because of this, there is **no guard, no manifest, and no runtime macro-fingerprint
check** — a `-D` against embedded headers is documented user error against a mode
boundary, not a case to detect.

Production also gains a deployment property: no system include dir is shipped or
trusted, so the target's `/usr/include` version/attack surface is simply absent.

---

## 7. Versioning

- **Release tag = clang major/minor** (e.g. `madc 19.x`; madc owns the patch
  component). One global, legible identity ("qualifies against the LLVM 19 feature
  level"). It is **not** a stdlib version claim and **not** a behavioral-model
  guarantee.
- **stdlib provenance = per-target stamp** in the per-target predefines table
  (glibc 2.x, libc++ 19, etc.). **One stdlib family per target.**
- `--no-embedded-headers` matches installed headers against the **stamp**, never the
  tag. (Matching the tag would falsely mismatch glibc 2.39 against tag 19.)
- Tag and stamp **coincide** on libc++ targets and **diverge** on glibc/libstdc++
  targets — correct, not a defect. The tag describes the madc release; the stamp
  describes the embedded library.

### One stdlib family per target

Supporting multiple families (glibc *and* musl, libstdc++ *and* libc++) on one
target was rejected: it multiplies the largest embedded artifact (contradicts
compact), pushes snapshot selection to compile time (hurts Fast), and forces
`--no-embedded-headers` to *detect* the installed family before matching (hurts
Robust — a detection branch that can be wrong, on the consistency path). The
alternate-stdlib capability is relocated to the live path (`--no-embedded-headers`
+ installed headers), which is exactly where Flexible lives and speed does not
matter. Want embedded musl *and* embedded glibc? That's two packs, not one fat
artifact — natural on the per-(arch, OS) packing axis.

---

## 8. Pack pipeline + qualification gate

Per **(arch, OS)** pack:

1. Generate the target's predefined-macro set **once**. Use that same table both to
   pre-parse the stdlib at pack time **and** to seed the user TU's PP at runtime —
   so baked headers and live user code cannot disagree on `__x86_64__` etc.
   (structural consistency, not a check).
2. Pre-parse stdlib → freeze arena → segment → set per-segment codec → compress →
   embed. Stamp with (clang release tag, per-target stdlib version).

**Deliberate pin, not auto-track.** Re-pinning a new LLVM release is a qualification
event: pull the new stdlib, run it through the frontend, fix what breaks, *then*
advance the pin. Automate the **watching** (open a qualification PR on upstream
release that reports what the frontend chokes on); keep the **gate** manual. This
matches the Codex-watches / human-gates working pattern and protects Robust —
upstream churn cannot break a build between madc releases, and the hardest-to-parse
C++ headers never enter the parser unqualified.

---

## 9. Per-segment codec field (load-bearing)

The per-segment codec flag is not merely a compression knob — it is the
**zero-copy knob** (§4). It governs two properties at once:

- `codec=none` → uncompressed, **resolved in place from the mmap** (zero-copy).
- `codec=zstd`/`lz4` → compressed on disk, **decompressed to arena on first touch**.

Default assignment: hot backbone segments → `codec=none`; cold side-car segments →
compressed. If profiling later shows per-segment decompress hurts even cold paths,
flip individual segments to `codec=none` with no format change. Carry this field
**from day one** — it is the escape hatch and it costs one byte per segment.

---

## Open / deferred

- `madc --version` surface: whether the behavioral-model coordinate ("models clang
  through N") is shown separately from the release tag, or the single tag string is
  the only user-visible identity.
- Concrete codec choice (zstd level vs lz4) — decode-speed benchmark on real stdlib
  segments, not decided here.
