# Multiple/Virtual Inheritance — S2: Live Layout (parse + flatten + lowering + this-adjust) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `compute_layout()` the LIVE, single layout authority for classes — parse multiple + `virtual` bases, flatten base members with origin tags, compute Itanium offsets, emit a C11 struct whose named fields land at those offsets, and adjust `this` by the base offset on base-method calls — proven on user-defined MI/virtual classes against g++, with single-inheritance classes byte-identical (zero regression).

**Architecture:** Approach decided 2026-06-03 = **origin-tagged flatten + `apply_member_layout`**. Keep early base-member flatten (so inherited-member resolution in method bodies is unchanged), but tag each flattened member with the base it came from. After the body loop, `compute_layout()` computes the metadata (base offsets, vbase offsets, secondary vptrs, `nvsize`, `size`, `own_block_off`) and `apply_member_layout()` rewrites every member's final offset from its origin — replacing the old `+8` vptr fix-up. The C11 emitter drives field+padding emission from the final `member_offsets` so c2mir reproduces the Itanium layout. Member access is `N_DEREF_FIELD` (named field), so faithful field offsets are mandatory.

**Tech Stack:** C++11; `include/datadef.h` (`DataDefCLASS`/`DataDefSTRUCT`), `src/parser.cpp` (`TokenCLASS::parse`, `compute_layout`), `src/cir_builder.cpp` (`class_struct_def`, `class_this_arg`/method-call this-adjust), doctest + integration `.mad` tests compared to g++.

**Spec:** `docs/superpowers/specs/2026-06-03-multiple-inheritance-design.md` (Part 1 §3 flatten, §4 lowering; Part 2 §7 this-adjust). **S1 (done):** `compute_layout` + `bases`/`vbase_offset`/`secondary_vptr_owners`/`nvsize` exist and are unit-tested; `compute_layout` is currently DORMANT (no live call site) and shifts own-member offsets itself.

**Scope note (vs spec §13):** S2 delivers MI/virtual for **user-defined** classes end-to-end at the *non-virtual-dispatch* level (layout + data-member access + non-virtual base-method `this`-adjust). **Virtual dispatch across MI = S3.** **Dropping `sizeof(std::)` and resolving `std::ofstream::good()` = the campaign's job** once it defines header std:: types using this machinery — NOT S2. S2's std:: types stay the existing builtins, untouched; the gate stays 469.

**Live-parser facts (verified):**
- Base parse + single-base flatten: `src/parser.cpp` ~11640-11680 (`// Copy base class members (at offset 0)`, `ddc->base_class = inherit_base;`, loop over `inherit_base->members`).
- Base list syntax parse (`: public Base`): ~11560-11590 (access specifier parsed then discarded; no `virtual`; no comma list).
- vptr `+8` fix-up after `}`: `src/parser.cpp` 11986-11994 (`if ( ddc->has_vtable && !(inherit_base && inherit_base->has_vtable) )` → `size += 8; member_offsets[i] += 8`).
- C11 struct emission: `src/cir_builder.cpp:2766` `class_struct_def` (`__vptr`@0 then members in `members` order; opaque `_w[words]` blob when no members).
- Member access: `N_DEREF_FIELD` named field (`src/cir_builder.cpp:3098,3121`).
- Method-call `this`: `class_this_arg` (`src/cir_builder.cpp:2860`) = bare `&obj`; inherited-method `this` = pure `(Base*)` cast, NO byte adjust (~2992-3004 region; `class_behind`@2829, `recv_class`).

---

## File Structure
- **`include/datadef.h`** — add `std::vector<int> member_origin;` (per-member: base index, or -1 = own) to `DataDefSTRUCT` (alongside the other parallel member vectors); add `size_t own_block_off;` + `void apply_member_layout();` to `DataDefCLASS`. Refactor `compute_layout()` to metadata-only (stop shifting `member_offsets`; set `own_block_off`).
- **`src/parser.cpp`** — (a) `compute_layout()` refactor + new `apply_member_layout()`; (b) base-list parse: `virtual` + comma-separated bases → `bases`; (c) origin-tagged flatten over all `bases`; (d) wire `compute_layout()`+`apply_member_layout()` after `}`, delete the `+8` fix-up.
- **`src/cir_builder.cpp`** — (a) `class_struct_def`: emit fields in `member_offsets` order with explicit `char __padN[k]` padding + `void *__vptrN` at secondary/vptr offsets so c2mir reproduces the layout; (b) base-method `this`-adjust: `(char*)this + base.offset`.
- **`tests/unit/test_class_layout.cpp`** — update for the metadata-only refactor (call `apply_member_layout`).
- **`tests/test_mi_*.mad`** (create) — integration tests for MI/virtual user classes, `.expect` matched to g++.

---

## Task 1: Refactor compute_layout to metadata-only + add apply_member_layout

**Files:** Modify `include/datadef.h`, `src/parser.cpp`; update `tests/unit/test_class_layout.cpp`.

- [ ] **Step 1: Update the S1 unit tests to the new two-call shape (failing)**

In `tests/unit/test_class_layout.cpp`, the `member_offsets` assertions must run through `apply_member_layout`. For each case that checks `member_offsets`, after `compute_layout()` add a default all-own origin and call `apply_member_layout()`. Example — change the `B : A` case body's tail:
```cpp
        b->bases.push_back(BaseSpec{a, 0, false, 0u, false});
        b->compute_layout();
        b->member_origin.assign(b->members.size(), -1); // all own (no flatten in unit test)
        b->apply_member_layout();
        CHECK(b->bases[0].offset == 0);
        CHECK(b->size == 16);
        CHECK(b->nvsize == 16);
        CHECK(b->member_offsets[0] == 8);
```
Apply the same two-line insertion (`member_origin.assign(...); apply_member_layout();` right after `compute_layout()`) to every case that asserts a `member_offsets[...]` value (the `B:A`, polymorphic-vptr, `Leaf`, `MIc`, `diamond` cases). Cases that only check `size`/`nvsize`/`offset`/`vbase_offset` need no change.

- [ ] **Step 2: Run to verify it fails (apply_member_layout / member_origin undefined)**

Run: `( ulimit -t 300; timeout 360 make -C src ../bin/test_class_layout )`
Expected: COMPILE FAIL — `member_origin` / `apply_member_layout` not declared.

- [ ] **Step 3: Add member_origin + own_block_off + apply_member_layout; make compute_layout metadata-only**

In `include/datadef.h`, in `DataDefSTRUCT`'s member-vector group (near `member_access`), add:
```cpp
    std::vector<int> member_origin; // per-member: base index it came from, or -1 = own (MI flatten)
```
In `DataDefCLASS` (near `nvsize`), add:
```cpp
    size_t own_block_off; // offset where this class's own data members begin
    void apply_member_layout(); // rewrite member_offsets from member_origin + the computed layout
```
Add `own_block_off(0)` to the ctor init list (after `nvsize(0)`).

In `src/parser.cpp` `compute_layout()`, replace the step-3 own-member block (the loop that does `member_offsets[i] = own_block + member_offsets[i]`) so it ONLY records the boundary, not rewrites members:
```cpp
    // 3. own data members begin after the non-virtual bases. Record the boundary;
    //    apply_member_layout() does the actual member_offsets rewrite (it knows each
    //    member's origin). max_align/own packed size come from addMember.
    if ( max_align > maxalign ) maxalign = max_align;
    own_block_off = mi_align_up(cur, max_align ? max_align : 1);
    size_t own_packed = members_own_packed_size(); // helper below
    cur = own_packed ? own_block_off + own_packed : own_block_off;
```
Add a small helper just above `compute_layout` to compute the packed size of OWN members (origin -1) — or, when `member_origin` is empty (unit-test/own-only case), the whole `size`:
```cpp
size_t DataDefCLASS::members_own_packed_size() const
{
    if ( member_origin.empty() ) return size; // own-only (no flatten): `size` is the own packed size
    size_t own = 0;
    for ( size_t i = 0; i < members.size(); i++ )
        if ( i < member_origin.size() && member_origin[i] == -1 )
        {
            size_t e = member_offsets[i] + member_counts[i] * members[i].second->size; // end of this member (own-local offset)
            if ( e > own ) own = e;
        }
    return own;
}
```
Declare `size_t members_own_packed_size() const;` in `DataDefCLASS`.

Add `apply_member_layout()` in `src/parser.cpp` after `compute_layout()`:
```cpp
// Rewrite each member's final offset from its origin: own members sit at
// own_block_off + their own-local offset; an inherited member sits at its base's
// subobject offset + its offset-within-that-base. Requires member_origin populated
// (size == members.size()); a -1 entry, or an empty member_origin, means "own".
void DataDefCLASS::apply_member_layout()
{
    for ( size_t i = 0; i < member_offsets.size(); i++ )
    {
        int origin = (i < member_origin.size()) ? member_origin[i] : -1;
        if ( origin < 0 )
            member_offsets[i] = own_block_off + member_offsets[i];
        else
        {
            // member_offsets[i] currently holds the member's offset WITHIN its base
            // (copied verbatim at flatten time); rebase onto the base subobject.
            size_t boff = (origin < (int)bases.size())
                            ? (bases[origin].is_virtual
                                 ? vbase_offset[bases[origin].base]
                                 : bases[origin].offset)
                            : 0;
            member_offsets[i] = boff + member_offsets[i];
        }
    }
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `( ulimit -t 300; timeout 360 make -C src ../bin/test_class_layout )` then `./bin/test_class_layout`
Expected: PASS — all 7 cases (now via compute_layout + apply_member_layout) green.

- [ ] **Step 5: Commit**

```bash
git add include/datadef.h src/parser.cpp tests/unit/test_class_layout.cpp
git commit -m "refactor(class): compute_layout metadata-only + apply_member_layout (S2 task 1)"
```

---

## Task 2: Parse `virtual` + comma-separated bases into `bases`

**Files:** Modify `src/parser.cpp` (base-list parse ~11560-11590); Test: `tests/test_mi_parse.mad` (create).

- [ ] **Step 1: Write the failing integration test**

Create `tests/test_mi_parse.mad`:
```cpp
#!/../bin/madc
// MI parse smoke: a class with two bases + a virtual base must parse and run.
class A { public: int a; };
class B { public: int b; };
class C : public A, public B { public: int c; };
class V { public: int v; };
class D : virtual public V { public: int d; };

int main()
{
    C obj;
    obj.a = 1; obj.b = 2; obj.c = 3;
    D dobj;
    dobj.v = 9; dobj.d = 4;
    printf("%d %d %d %d %d\n", obj.a, obj.b, obj.c, dobj.v, dobj.d);
    return 0;
}
```
Create `tests/test_mi_parse.expect`:
```
1 2 3 9 4
```

- [ ] **Step 2: Run to verify it fails (parse error on `,`/`virtual`)**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_parse.mad )`
Expected: FAIL — parse error at the comma (`Expected base class name`) or at `virtual`.

- [ ] **Step 3: Extend the base-list parse**

In `src/parser.cpp`, replace the single-base parse (the `if ( tn->id() == TokenID::tkColon )` block ~11560-11590) with a loop that reads a comma-separated list, each entry optionally `virtual` and an access specifier, pushing a `BaseSpec` per base and keeping `inherit_base`=first base for the existing single-base flatten/compat (the multi-base flatten lands in Task 3):
```cpp
    DataDefCLASS *inherit_base = NULL;
    if ( tn->id() == TokenID::tkColon )
    {
        pgm.nextToken(); // consume ':'
        do {
            bool bvirtual = false;
            uint32_t baccess = 0;
            // `virtual` and the access specifier may appear in either order.
            for (;;) {
                TokenBase *kw = pgm.peekToken();
                if ( kw && kw->type() == TokenType::ttIdentifier ) {
                    std::string &s = ((TokenIdent *)kw)->str;
                    if ( s == "virtual" ) { bvirtual = true; pgm.nextToken(); continue; }
                    if ( s == "public" )    { baccess = 0;            pgm.nextToken(); continue; }
                    if ( s == "protected" ) { baccess = vfPROTECTED;  pgm.nextToken(); continue; }
                    if ( s == "private" )   { baccess = vfPRIVATE;    pgm.nextToken(); continue; }
                }
                break;
            }
            TokenBase *bn = pgm.nextToken();
            if ( !bn || bn->type() != TokenType::ttIdentifier )
                pgm.Throw(bn ? bn : tag) << "Expected base class name" << flush;
            std::string base_name = ((TokenIdent *)bn)->str;
            auto dmi = pgm.datatype_map.find(base_name);
            DataDefCLASS *bcls = (dmi != pgm.datatype_map.end())
                ? dynamic_cast<DataDefCLASS *>(dmi->second) : NULL;
            if ( !bcls )
                pgm.Throw(bn) << "Unknown base class '" << base_name << "'" << flush;
            ddc->bases.push_back(BaseSpec{bcls, 0, bvirtual, baccess, false});
            if ( !inherit_base ) inherit_base = bcls;
        } while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkComma
                  && (pgm.nextToken(), true) );
        tn = pgm.peekToken();
    }
```
(Keep `ddc->base_class = inherit_base;` in the existing flatten block — Task 3 reworks the flatten body.)

- [ ] **Step 4: Run — expect it to RUN (single-base flatten still only handles base 0, but A/B members of C and V of D need Task 3; for now C.b / D.v may be wrong). Confirm it at least PARSES (no parse error).**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_parse.mad )`
Expected: NO parse error (it compiles + runs). Output may be partially wrong (`obj.b`/`dobj.v` not yet laid out) — that is fixed in Task 3/4. Do NOT add to the runner's pass set yet.

- [ ] **Step 5: Commit**

```bash
git add src/parser.cpp tests/test_mi_parse.mad tests/test_mi_parse.expect
git commit -m "feat(parser): parse virtual + comma-separated base lists into bases (S2 task 2)"
```

---

## Task 3: Origin-tagged flatten over all bases

**Files:** Modify `src/parser.cpp` (flatten block ~11647-11680).

- [ ] **Step 1: (covered by test_mi_parse.mad)** The Task-2 test will produce correct `obj.b`/`dobj.v` only once all bases are flattened with origins. Keep it out of the pass set until Task 4 wires layout.

- [ ] **Step 2: Run to confirm current wrong behavior**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_parse.mad )`
Expected: runs but prints wrong values for the 2nd-base/virtual-base members (only base 0 flattened).

- [ ] **Step 3: Rework the flatten to loop over all bases with origin tags**

Replace the single-base member-copy loop (~11655-11675) with a loop over `ddc->bases`, copying each base's members and recording origin = base index, and seeding `member_origin` for own members as they are added. First, after `ddc->base_class = inherit_base;`, flatten every base:
```cpp
    // Flatten EACH base's data members into the derived class so method bodies can
    // resolve inherited members during the body loop. Tag each with its origin base
    // index; final offsets are assigned by compute_layout()+apply_member_layout()
    // after the body loop. member_offsets here hold the member's offset WITHIN its base.
    for ( size_t bi = 0; bi < ddc->bases.size(); bi++ )
    {
        DataDefCLASS *b = ddc->bases[bi].base;
        for ( size_t i = 0; i < b->members.size(); ++i )
        {
            ddc->members.push_back(b->members[i]);
            ddc->member_offsets.push_back(b->member_offsets[i]);
            ddc->member_counts.push_back(i < b->member_counts.size() ? b->member_counts[i] : 1);
            ddc->member_array_flags.push_back(i < b->member_array_flags.size() ? b->member_array_flags[i] : false);
            ddc->member_bitfields.push_back(i < b->member_bitfields.size() ? b->member_bitfields[i] : BitFieldInfo());
            ddc->member_dims.push_back(i < b->member_dims.size() ? b->member_dims[i] : std::vector<uint32_t>());
            ddc->member_count_exprs.push_back(i < b->member_count_exprs.size() ? b->member_count_exprs[i] : NULL);
            ddc->member_access.push_back(i < b->member_access.size() ? b->member_access[i] : 0);
            ddc->member_origin.push_back((int)bi);
        }
        // inherit methods / vtable / dtor flags from each base (was single-base before)
        for ( auto &mp : b->method_map )
            if ( ddc->method_map.find(mp.first) == ddc->method_map.end() )
                ddc->method_map[mp.first] = mp.second;
        if ( b->has_user_dtor ) ddc->has_user_dtor = true;
        if ( b->has_vtable ) {
            ddc->has_vtable = true;
            for ( auto &vs : b->vtable_slots )
                if ( ddc->vtable_slot(vs) < 0 ) ddc->vtable_slots.push_back(vs);
            for ( auto &vm : b->virtual_methods ) ddc->virtual_methods[vm.first] = true;
        }
    }
    ddc->size = 0; // own members start at 0; compute_layout assigns the real total
```
Delete the old single-base copy loop + `ddc->size = inherit_base->size;` seed (now replaced above). **Important:** `addMember` (own members in the body loop) does NOT push to `member_origin`, so add `ddc->member_origin.push_back(-1);` right after each own `addMember` call in the body loop (the data-member branch ~11970) — OR, simpler, normalize at the call site in Task 4 step 3 by resizing `member_origin` to `members.size()` filling new entries with -1 before `apply_member_layout()`. Use the resize-normalize approach (one line, no scatter): documented in Task 4.

- [ ] **Step 4: Run — still possibly wrong offsets until compute_layout is wired (Task 4). Confirm it builds + runs without crashing.**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_parse.mad )`
Expected: builds + runs (offsets become correct only after Task 4). No crash.

- [ ] **Step 5: Commit**

```bash
git add src/parser.cpp
git commit -m "feat(parser): origin-tagged flatten over all bases (S2 task 3)"
```

---

## Task 4: Wire compute_layout + apply_member_layout live; delete the +8 fix-up

**Files:** Modify `src/parser.cpp` (after `}` ~11986-11994).

- [ ] **Step 1: (test_mi_parse.mad becomes correct here)** Add it to the pass set only after this task is green (Task 7 confirms via the runner).

- [ ] **Step 2: Run to confirm pre-wire state**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_parse.mad )` ; compare to g++:
```bash
cat > tmp/mi_ref.cpp <<'EOF'
#include <cstdio>
struct A { int a; }; struct B { int b; };
struct C : A, B { int c; };
struct V { int v; }; struct D : virtual V { int d; };
int main(){ C o; o.a=1;o.b=2;o.c=3; D d; d.v=9; d.d=4;
  printf("%d %d %d %d %d\n",o.a,o.b,o.c,d.v,d.d); return 0; }
EOF
( ulimit -t 60; g++ -std=c++11 tmp/mi_ref.cpp -o tmp/mi_ref ) && ./tmp/mi_ref   # prints: 1 2 3 9 4
```
Expected: madc output differs from `1 2 3 9 4` (pre-wire).

- [ ] **Step 3: Replace the +8 fix-up with compute_layout + apply_member_layout**

In `src/parser.cpp`, replace the vptr `+8` fix-up block (11986-11994) with:
```cpp
    // Finalize layout: compute_layout assigns base/vbase offsets, vptr placement,
    // nvsize/size and own_block_off; apply_member_layout rewrites every member's
    // final offset from its origin. This REPLACES the old +8 vptr fix-up and gives
    // correct non-zero offsets for multiple/virtual bases. For single inheritance it
    // reproduces the old layout byte-for-byte (vptr@0, base@0, own members after).
    ddc->member_origin.resize(ddc->members.size(), -1); // own members (added via addMember) -> -1
    ddc->compute_layout();
    ddc->apply_member_layout();
```
(Leave the vtable allocation block at 11997-12002 unchanged.)

- [ ] **Step 4: Run to verify the MI test now matches g++**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_parse.mad )`
Expected: prints `1 2 3 9 4` (matches g++).

Then the regression gate — single inheritance must be byte-identical:
Run: `( ulimit -t 600; timeout 700 bash scripts/run_tests.sh > tmp/s2t4.log 2>&1 ); tail -3 tmp/s2t4.log`
Expected: `457 passed` (same baseline set: the known 6 fails + flaky `testfortypedcomma`). If ANY new test regresses, STOP — the single-inheritance path diverged; diff its computed offsets vs the old `+8` result before proceeding.

- [ ] **Step 5: Commit**

```bash
git add src/parser.cpp
git commit -m "feat(class): compute_layout is the live layout authority; delete +8 vptr fix-up (S2 task 4)"
```

---

## Task 5: Offset-ordered C11 struct emission

**Files:** Modify `src/cir_builder.cpp` (`class_struct_def` 2766).

- [ ] **Step 1: Write the failing integration test (member offset / sizeof faithfulness)**

Create `tests/test_mi_layout.mad`:
```cpp
#!/../bin/madc
class P1 { public: long p1; };
class P2 { public: long p2; };
class MIc : public P1, public P2 { public: long c; };
int main()
{
    MIc m;
    m.p1 = 11; m.p2 = 22; m.c = 33;
    // distinct addresses prove P2 is NOT aliased onto P1 (non-zero 2nd-base offset)
    printf("%ld %ld %ld %d\n", m.p1, m.p2, m.c, (int)sizeof(MIc));
    return 0;
}
```
Create `tests/test_mi_layout.expect`:
```
11 22 33 24
```
(g++ ref: `struct P1{long p1;};struct P2{long p2;};struct MIc:P1,P2{long c;};` → these are non-polymorphic, so sizeof(MIc)=24, p1@0,p2@8,c@16.)

- [ ] **Step 2: Run to verify it fails**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_layout.mad )`
Expected: wrong sizeof or aliased members — because `class_struct_def` emits members in `members` order without honoring `member_offsets`, so c2mir's natural field layout may not equal the computed offsets.

- [ ] **Step 3: Drive struct emission from final member_offsets with explicit padding**

In `src/cir_builder.cpp` `class_struct_def`, replace the "vptr first, then members in order" emission with an **offset-sorted** emission that inserts `char __padN[k]` filler and `void *__vptrN` so c2mir reproduces the computed offsets. Build a list of (offset, kind, member-index) entries: the primary `__vptr` at 0 if `has_vtable`; a `void*` vptr at each `secondary_vptr_owners` offset (look up via `bases[].offset` for the owner); each data member at its `member_offsets[i]`; then sort by offset and emit, inserting `char __padN[gap]` whenever the next field's offset exceeds the running cursor. Pseudocode-free concrete shape:
```cpp
    struct Field { size_t off; int kind; size_t midx; }; // kind: 0=vptr,1=member
    std::vector<Field> fields;
    if (cdd->has_vtable) fields.push_back({cdd->primary_vptr_off, 0, 0});
    for (DataDefCLASS *o : cdd->secondary_vptr_owners) {
        // find this owner's base offset
        for (auto &bs : cdd->bases) if (bs.base == o) { fields.push_back({bs.offset, 0, 0}); break; }
    }
    for (size_t i = 0; i < cdd->members.size(); i++)
        fields.push_back({cdd->member_offsets[i], 1, i});
    std::sort(fields.begin(), fields.end(), [](const Field&a, const Field&b){ return a.off < b.off; });
    size_t cursor = 0; int padn = 0;
    for (auto &f : fields) {
        if (f.off > cursor) { /* emit char __padN[f.off-cursor]; */ append(member_list, pad_member(f.off-cursor, padn++)); cursor = f.off; }
        if (f.kind == 0) { append(member_list, vptr_member_named(padn /*reuse counter for unique name*/)); cursor += 8; }
        else { append(member_list, member_node(cdd->members[f.midx], cdd)); cursor += cdd->member_counts[f.midx] * cdd->members[f.midx].second->size; }
    }
    if (cdd->size > cursor) append(member_list, pad_member(cdd->size - cursor, padn++)); // tail pad to full size
```
Add two small local emitters: `pad_member(size_t n, int id)` → a `char __pad<id>[n];` `N_MEMBER`, and `vptr_member_named(int id)` → a `void *__vptr<id>;` `N_MEMBER` (the existing `__vptr` emission, parameterized by a unique name; the primary keeps `__vptr` so existing vptr loads at offset 0 still resolve). Keep the opaque `_w[words]` path for the no-madc-members std:: case unchanged. (Member access is by name via `N_DEREF_FIELD`, so the member nodes keep their real names; padding/vptr names are never referenced by user code.)

- [ ] **Step 4: Run to verify it passes + single-inheritance regression**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_layout.mad )`
Expected: `11 22 33 24`.
Run: `( ulimit -t 600; timeout 700 bash scripts/run_tests.sh > tmp/s2t5.log 2>&1 ); tail -3 tmp/s2t5.log`
Expected: `457 passed` baseline (the offset-ordered emitter must produce the SAME struct for single-inheritance classes — primary vptr@0 then members in ascending offset, no pads needed).

- [ ] **Step 5: Commit**

```bash
git add src/cir_builder.cpp tests/test_mi_layout.mad tests/test_mi_layout.expect
git commit -m "feat(cir): offset-ordered class struct emission for MI layout (S2 task 5)"
```

---

## Task 6: Compile-time this-adjust on base-method calls

**Files:** Modify `src/cir_builder.cpp` (inherited-method `this` cast ~2992-3004).

- [ ] **Step 1: Write the failing integration test**

Create `tests/test_mi_method.mad`:
```cpp
#!/../bin/madc
class P1 { public: long p1; long getp1() { return this.p1; } };
class P2 { public: long p2; long getp2() { return this.p2; } };
class MIc : public P1, public P2 { public: long c; };
int main()
{
    MIc m;
    m.p1 = 100; m.p2 = 200; m.c = 300;
    // getp2() is defined in P2 (subobject @ offset 8): this must be adjusted +8
    printf("%ld %ld\n", m.getp1(), m.getp2());
    return 0;
}
```
Create `tests/test_mi_method.expect`:
```
100 200
```

- [ ] **Step 2: Run to verify it fails**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_method.mad )`
Expected: `m.getp2()` returns wrong value (likely 100 or garbage) — the inherited-method `this` is a bare cast with no +8 adjust, so P2's method reads at the wrong offset.

- [ ] **Step 3: Adjust `this` by the base subobject offset**

In `src/cir_builder.cpp`, at the inherited-method `this` handling (the `owner != recv_class` cast ~2992-3004), compute the base offset and add it when non-zero. Find which `BaseSpec` of `recv_class` the `owner` corresponds to (direct or transitive — walk to find the offset; for a direct base it is `bases[i].offset` or `vbase_offset[owner]`):
```cpp
    DataDefCLASS *owner = (callee && !callee->parameters.empty())
                ? class_behind(callee->parameters[0]) : NULL;
    if (owner && owner != recv_class) {
        size_t boff = recv_class->base_offset_of(owner); // helper: subobject offset, 0 if not found/primary
        node_t adjusted = this_arg;
        if (boff != 0) {
            // (char*)this + boff, then cast to owner*
            node_t bytep = node2(N_CAST, char_ptr_type(), this_arg, origin);
            adjusted = node2(N_ADD, bytep, integer((long)boff), origin);
        }
        this_arg = node2(N_CAST, /* owner* */ class_ptr_type(owner), adjusted, origin);
    }
```
Add `size_t DataDefCLASS::base_offset_of(const DataDefCLASS *target) const` in `src/parser.cpp` (returns the subobject offset of `target` within this class, searching direct bases then their subobjects transitively, consulting `vbase_offset` for virtual bases; returns 0 if `target==this` or not found):
```cpp
size_t DataDefCLASS::base_offset_of(const DataDefCLASS *target) const
{
    if ( target == this ) return 0;
    for ( const auto &bs : bases ) {
        size_t base = bs.is_virtual ? vbase_offset.at(bs.base) : bs.offset;
        if ( bs.base == target ) return base;
        // transitive: offset within the base, added to the base's own offset
        size_t inner = bs.base->base_offset_of(target);
        if ( inner != 0 || bs.base == target ) return base + inner;
    }
    return 0;
}
```
Declare it in `DataDefCLASS`. (Use `char_ptr_type()`/`class_ptr_type()` — confirm the existing helpers' names in cir_builder.cpp during implementation; reuse the pointer-type builders already used by the bare-cast path.)

- [ ] **Step 4: Run to verify it passes + regression**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_method.mad )`
Expected: `100 200`.
Run: `( ulimit -t 600; timeout 700 bash scripts/run_tests.sh > tmp/s2t6.log 2>&1 ); tail -3 tmp/s2t6.log`
Expected: `457 passed` baseline (offset 0 for single-inheritance bases → byte-identical to today's bare cast).

- [ ] **Step 5: Commit**

```bash
git add src/cir_builder.cpp src/parser.cpp tests/test_mi_method.mad tests/test_mi_method.expect
git commit -m "feat(cir): compile-time this-adjust on base-method calls (S2 task 6)"
```

---

## Task 7: Full S2 gate

**Files:** none (verification + push).

- [ ] **Step 1: Build clean (0 warnings)**

Run: `( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )`
Expected: `0`.

- [ ] **Step 2: Unit tests green**

Run: `( ulimit -t 300; timeout 360 make -C src ../bin/test_class_layout ) && ./bin/test_class_layout | tail -3`
Expected: PASS (7 cases via the metadata-only refactor).

- [ ] **Step 3: Integration baseline + the 3 new MI tests pass**

Run: `( ulimit -t 600; timeout 700 bash scripts/run_tests.sh > tmp/s2gate.log 2>&1 ); grep -E "passed|test_mi_" tmp/s2gate.log | tail -8`
Expected: `460 passed` (baseline 457 + the 3 new `test_mi_parse`/`test_mi_layout`/`test_mi_method`); the SAME 6 known fails + flaky `testfortypedcomma`. No other regression.

- [ ] **Step 4: SMAUG soak (parser/codegen change → must still boot)**

Run: `cd /workspace/MadSMAUG/runtime/area; timeout 50 /workspace/madc/bin/madc /workspace/MadSMAUG/src/SMAUG.mad 40NN > /workspace/madc/tmp/smaug-s2.log 2>&1; echo $?` then `grep -c "Realms of Despair ready at" /workspace/madc/tmp/smaug-s2.log ; pkill -9 -f 'bin/madc'`
Expected: the ready line present (SMAUG uses single-inheritance classes; layout must be byte-identical).

- [ ] **Step 5: Gate unchanged + push**

Run: `bash scripts/check-no-std-hardcoding.sh | head -1`
Expected: `469` (S2 touches no std:: hardcoding — std:: types stay the builtins).
```bash
git push
```

---

## Self-review notes (author)
- **Spec coverage:** S2 covers Part 1 §3 (origin-tagged flatten) + §4 (offset-faithful C11 emission) + Part 2 §7 compile-time this-adjust, for USER classes. Virtual DISPATCH across MI = S3 (not here). `sizeof(std::)` removal + `good()` = campaign (explicitly out of scope; gate stays 469).
- **Type/name consistency:** `member_origin` (std::vector<int>, -1=own), `own_block_off`, `apply_member_layout()`, `members_own_packed_size()`, `base_offset_of()` are declared in Task 1/3/6 and used consistently. `BaseSpec{base,offset,is_virtual,access,is_primary}` order matches S1.
- **Regression discipline:** every behavior-changing task (4,5,6) re-runs `run_tests.sh` and must hold the 457 baseline; single inheritance is byte-identical by construction (primary vptr@0, base@0, ascending offsets, no pads). SMAUG soak in Task 7.
- **Unverified-helper flag:** Task 5/6 reference pointer-type builders (`char_ptr_type`/`class_ptr_type`/`pad_member`/`vptr_member_named`) — confirm/adapt to the actual cir_builder helper names during implementation (they wrap existing patterns: `void_ptr_type()` exists at the bare-cast path; `N_ARR` char arrays exist in the `_w[words]` path). Not a placeholder for logic — only the helper spelling is to be matched to the file.
- **member_origin normalization:** own members added via `addMember` don't push to `member_origin`; Task 4 step 3 resizes `member_origin` to `members.size()` filling -1 right before `apply_member_layout()` — single point, no scattered pushes.
