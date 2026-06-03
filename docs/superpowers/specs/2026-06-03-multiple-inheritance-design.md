# Multiple & Virtual Inheritance — Design Spec (2026-06-03)

**Goal:** Give madc faithful Itanium-C++-ABI multiple and virtual inheritance —
both the layout model (sizes, base-subobject offsets, vptr placement) and the
full polymorphic codegen (vtables, VTT/construction vtables, this-adjusting
thunks, runtime virtual-base offset lookup, RTTI/`dynamic_cast`) — for
user-defined classes, lowered to portable C11 for c2mir, and interoperating with
the real libstdc++.

**Why now:** The retire-std-hardcoding campaign needs `std::basic_ios::good()`
to resolve at its real virtual-base offset (+248 in `ofstream`, +256 in
`ifstream`) WITHOUT hardcoding the offset or `sizeof(std::…)`. madc's class model
is single-inheritance, base-at-offset-0 only. The correct, deepest-layer fix is
real MI/virtual inheritance — which madc needs for proper C++ support regardless.
(User: "we're going to need it anyways for proper C++ support… do it right.")

**Scope (decided):** Full Tier A + Tier B (see below). One model — the
Itanium-faithful scheme REPLACES madc's current simplified single-vptr scheme
(no parallel implementations).

**Branch:** `feature/multiple-inheritance-claude`, cut from
`feature/retire-std-hardcoding-claude` HEAD (builds on the completed mangler;
zero duplication = least drift). Merges back into the campaign branch when green;
the stack promotes to develop together later. develop stays untouched.

---

## 0. Ground truth — Itanium ABI (probed on this toolchain)

Probe: `tmp/mi_probe.cpp`; `clang++ -Xclang -fdump-record-layouts` /
`-fdump-vtable-layouts`; runtime `static_cast` offsets. Findings archived in
`tmp/mi-abi-findings.md`.

**Layout algorithm:**
1. **Primary base** = first polymorphic non-virtual base → offset 0, SHARES the
   most-derived vptr.
2. **Other non-virtual bases** in declaration order; a non-primary polymorphic
   base gets its OWN vptr → starts at a non-zero offset.
3. **Own data members** after the non-virtual bases.
4. **Virtual bases** appended ONCE at the END (shared across a diamond), at
   `align(nvsize)`. `nvsize` = size of the non-virtual portion.

**Verified numbers:** `B:A`→A@0; `Leaf:Mid:virtual Vbase`→Mid@0,l@16,Vbase@24
(nvsize=24); `MIc:P1,P2`→P1@0,P2@16,c@32; `Diamond:L,R(:virtual Top)`→L@0,R@16,
d@32,Top@40. Streams: `ofstream→ostream@0,→basic_ios@248`;
`ifstream→istream@0,→basic_ios@256`. Sizes: ofstream 512, ifstream 520,
fstream 528, ostream 272, istream 280, basic_ios 264, ios_base 216, filebuf 240.

**vtable structure:** each subobject with its own vptr → a vtable address point;
prologue entries `vbase_offset`, `vcall_offset`, `offset_to_top`, RTTI, then
function slots; non-zero-offset overrides reached via this-adjusting thunks;
virtual-base construction needs construction vtables + VTT.

**Key insight (campaign):** madc binds std:: methods to libstdc++ symbols and
emits no bodies — libstdc++ owns construction and vtables. For statically-typed
objects (`ofstream outf; outf.good()` — the only shape the tests use) the
vbase offset is a COMPILE-TIME CONSTANT from the layout. So std:: needs only
correct SIZE + base OFFSETS; the full vtable/VTT/thunk codegen is required only
for USER-defined polymorphic MI classes.

---

## madc baseline (recon, read-only)

- **Base parse** (parser.cpp:11471-11496): exactly one base; `public/private/
  protected` parsed then DISCARDED; `virtual` NOT handled; no comma list.
- **Base flatten** (parser.cpp:11559-11595): base members copied at original
  offsets (offset 0); `size` seeded to base size; inherits vtable_slots/
  virtual_methods/method_map. BUG: only `members`/`member_offsets`/
  `member_array_flags` copied — `member_counts`/`member_bitfields`/`member_dims`/
  `member_access`/`member_count_exprs`/`member_explicit_align` desync.
- **vtable** (parser.cpp:11842-11913, cir_builder.cpp:2749-2763, 3027-3064):
  ONE `__vptr` @0, ONE flat `Class__vtable[]`, dispatch `((void**)__vptr)[slot]`.
  Simplified single-vptr — no secondary vptrs, thunks, VTT, vbase_offset.
- **C11 emission** (cir_builder.cpp:2766 `class_struct_def`): `__vptr`@0 if
  has_vtable, then flattened members; opaque `long _w[words]` blob (sized from
  `cdd->size`, today via `sizeof(std::…)`) when no madc members.
- **`this`** (cir_builder.cpp:2860, 2992-3004): plain `&obj`; inherited base
  method `this` = pure `(Base*)` cast, NO byte adjust (relies on offset 0).
- **The one non-zero-offset handling today**: `sstream_ostream` runtime
  `static_cast` wrapper (madc_mir_backend.cpp:235) — itself a campaign-deleted shim.
- **~16 sites assume base-at-offset-0** (full table in `tmp/mi-abi-findings.md` /
  the recon report).

---

## Part 1 — Object model & layout engine (Tier A)

### §1 Data structures (`include/datadef.h` DataDefCLASS)
Replace single `base_class` with a base list recording offset + virtuality +
access:
```cpp
struct BaseSpec {
    DataDefCLASS *base;
    size_t        offset;     // subobject offset within THIS class
    bool          is_virtual;
    uint32_t      access;     // existing vf* flags (0=public/vfPRIVATE/vfPROTECTED); kept now (was discarded)
    bool          is_primary; // shares most-derived vptr (offset 0, polymorphic)
};
std::vector<BaseSpec> bases;                       // replaces base_class
std::map<DataDefCLASS*, size_t> vbase_offset;      // virtual base -> offset
size_t nvsize;                                     // non-virtual size
size_t primary_vptr_off = 0;
std::vector<DataDefCLASS*> secondary_vptr_owners;  // non-primary polymorphic bases
```
`base_class` becomes a compat accessor (`bases.empty()?NULL:bases[0].base`) so
the ~16 single-inheritance sites keep working during transition; migrated to the
offset-aware path as Tier B lands. `is_or_derives_from` walks the full graph and
dedups the diamond's shared virtual base.

### §2 Layout engine — `DataDefCLASS::compute_layout()`
One function, the single source of truth, run at the closing `}`, implementing
the §0 algorithm: primary base @0 (inherit vptr) or reserve `__vptr`@0 if
polymorphic-and-no-primary; non-virtual bases in order (non-primary polymorphic
→ own vptr → secondary_vptr_owner); own members; `nvsize=align(cur)`; append each
virtual base ONCE at nvsize (dedup); `size=align(end)`. **Replaces
`sizeof(std::…)`** (datadef.h:770, the `object_class_words` path) — madc derives
512/248 itself. Locked by a doctest that `#include`s real `<fstream>`/`<string>`
and asserts computed size/offset == libstdc++ `sizeof`/`offsetof`/`static_cast`.
madc ships no `sizeof(std::)`.

### §3 Base-member flattening rework (parser.cpp:11559-11595)
After `compute_layout()`, flatten EACH base's members at `member_offset +
base.offset` (non-zero for secondary/virtual bases), copying ALL parallel vectors
in lockstep (fixes the desync bug). Shared virtual base flattened once.

### §4 C11 object-model lowering (cir_builder.cpp:2766)
Extend the flatten-into-one-struct emission (keeps current member-access codegen
valid): primary `__vptr`@0 as today PLUS a `void *__vptr_N` at each secondary
owner's offset; virtual-base subobject fields appended at nvsize+; opaque std::
blob `_w[words]` sized from the COMPUTED layout, not `sizeof(std::)`.

---

## Part 2 — Vtables, VTT, thunks, this-adjust (Tier B)

### §5 Vtable groups (replaces parser.cpp:11842-11913, cir_builder.cpp:2749-2763,
3027-3064)
One grouped `Class__vtable[]` with multiple address points (primary, then each
secondary base, then virtual bases); each address point preceded by ABI prologue
entries (`vbase_offset`/`vcall_offset`/`offset_to_top`/RTTI), then function slots.
In C11: `void*` array; offsets are integers cast to `void*`; function slots are
the real method symbol or a thunk (§7). Each subobject's vptr is initialized
during construction to `&Class__vtable[address_point]`. **Dispatch shape
unchanged** — `((Fn*)((void**)vptr)[slot])(this,…)`; vptr already points at the
right address point, slot stays a small per-table index. What changes is vtable
construction (the grouped array) and setting each vptr to its address point.

### §6 VTT & construction vtables
Emit `Class__VTT[]` (vtable address points for base ctors) and construction
vtables for bases that themselves have vbases. Ctors take a hidden VTT-pointer
param in the base-subobject case (Itanium ABI), so madc-built objects interoperate
with libstdc++-built ones. std:: classes need no ctor body (libstdc++'s ctor does
this); only USER virtual-base classes need emitted VTT/construction vtables.

### §7 this-adjust + thunks
- **Compile-time-constant** (dominant; only case std:: needs): when the static
  type is known, replace the bare `(Base*)` cast (cir_builder.cpp:2992-3004,
  2356, 5547, 6918) with `(char*)this + base.offset`. Offset 0 = byte-identical
  to today.
- **Runtime `vbase_offset` lookup**: calling a virtual-base method through a base
  pointer of unknown most-derived type → `off=((long*)vptr)[vbase_idx];
  this=(char*)recv+off`.
- **This-adjusting thunks**: override reached through a non-primary base (offset
  ≠ 0) → vtable slot holds an emitted C function
  `Class__thunk_m(self,…){return Class__m((char*)self-K,…);}`. Covariant returns
  get a result-adjusting variant.

### §8 ctor/dtor across MI + ordering
Extend ctor/dtor chaining (cir_builder.cpp:6918-6943, 3139, 3365-3377): construct
all non-virtual bases in declaration order with offset-adjusted `this`, virtual
bases once (most-derived) in canonical order, members, self; destroy in reverse.
Virtual dtors dispatch via §5. `override`/`final` checked at parse; pure-virtual
→ class abstract (no instantiation; slot = `__cxa_pure_virtual`).

---

## Part 3 — RTTI, mangling, lowering, testing, sequencing

### §9 RTTI / `dynamic_cast` / `typeid`
Emit Itanium `type_info` objects (`__class_type_info` / `__si_class_type_info` /
`__vmi_class_type_info`) as C structs referencing the libsupc++ type_info
vtables; named `_ZTI<class>` (+ `_ZTS<class>` name), pointed to from each vtable's
RTTI slot. `dynamic_cast` → libstdc++ `__dynamic_cast(src,&srcTI,&dstTI,hint)`;
`typeid` → the type_info object. std:: classes REFERENCE libstdc++'s type_info via
the mangler (emit nothing); only USER classes emit type_info.

### §10 Mangling (extends src/madc_mangle.cpp — single symbol source)
Add + doctest (vs `c++filt`): `_ZTV` vtable, `_ZTT` VTT, `_ZTI` type_info, `_ZTS`
type-name, `_ZThn<n>_` non-virtual thunk, `_ZTv<n>_<n>_` virtual thunk, `_ZTC`
construction vtable.

### §11 C11 lowering / portability
All emitted constructs are portable C11 — `void*` arrays (offset entries as ints
cast to `void*`), thunks as `static` C functions, dispatch as function-pointer
calls. No inline asm / non-C constructs → `--emit=c11` stays valid; links against
libstdc++/libsupc++ exports (`__dynamic_cast`, `__cxa_pure_virtual`, type_info
vtables). Alignment follows computed `align`, doctest-verified.

### §12 Testing
- **Layout doctests** (no-hardcoding cross-check): `#include` real headers + probe
  classes; assert madc size/offset == libstdc++ `sizeof`/`offsetof`/`static_cast`.
- **Mangling doctests** vs `c++filt`.
- **Integration** `tests/*.mad`: single-virtual-base, MI non-virtual, diamond,
  this-adjust, virtual dtor across MI, `dynamic_cast`, abstract/pure-virtual; each
  `.expect` matched to g++ (gcc-parity).
- **Regression**: `make fulltest` green throughout + gcc-torture non-regressing.
  Riskiest: unifying existing simplified-vtable tests onto the Itanium model;
  managed by the `base_class` compat accessor + staged migration.

### §13 Staged build order (each stage: build clean + fulltest green + torture non-regressing)
- **S1 Tier A core:** data structures + `compute_layout()` + layout doctest. No
  behavior change (compat accessor); single-inheritance sizes unchanged, new
  MI/vbase sizes match g++.
- **S2 Tier A flatten + lowering:** flatten rework (all parallel vectors) +
  secondary-vptr/vbase C11 emission + compile-time this-adjust. ofstream/ifstream
  shape resolves `good()` at +offset.
- **S3 Tier B vtables:** grouped vtable arrays + secondary vptrs + address-point
  dispatch; retire the simplified model.
- **S4 Tier B construction:** VTT/construction vtables + MI ctor/dtor ordering +
  runtime vbase_offset + thunks.
- **S5 Tier B RTTI:** type_info + dynamic_cast + typeid.

Then the std:: campaign resumes on this foundation: **string-first** → **streams**
(`good()` via S2) → **conversions** → gate 0.

---

## Non-goals / YAGNI
- No exception-unwinding rework — madc exceptions stay SJLJ; RTTI here is for
  `dynamic_cast`/`typeid` only.
- No member-pointer-to-virtual nuances beyond what tests/SMAUG exercise (revisit
  if a test needs it).
- No re-litigating the backend (c2mir/C11 path per ADR 0001).
