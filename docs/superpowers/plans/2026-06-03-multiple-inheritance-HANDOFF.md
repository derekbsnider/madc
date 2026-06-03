# MULTIPLE/VIRTUAL INHERITANCE — HANDOFF (READ FIRST after compact/restart)

> Self-contained resume doc for the madc MI feature. Reading this + the spec + the
> per-stage plans + the memory `project_multiple_inheritance` = full situational
> awareness. Branch is separate from develop and from the std:: campaign.

## ⏩ STEP 0 — ORIENT
```
bash scripts/resume.sh                                 # live git/build state (its "NEXT" points at the PARITY track — IGNORE for this branch)
git -C /workspace/madc branch --show-current           # expect: feature/multiple-inheritance-claude
git -C /workspace/madc log --oneline develop..HEAD     # the MI + campaign-base commits
```
Authoritative docs: spec **`docs/superpowers/specs/2026-06-03-multiple-inheritance-design.md`**;
per-stage plans `docs/superpowers/plans/2026-06-03-multiple-inheritance-S{1,2,3,4}-*.md`;
ABI probe findings `tmp/mi-abi-findings.md` (gitignored — regenerate from `tmp/mi_probe.cpp`
via `clang++ -Xclang -fdump-record-layouts`/`-fdump-vtable-layouts`). Memory:
`project_multiple_inheritance` (full state), `project_string_as_class`,
`project_multiple_inheritance` links the campaign.

## WHY / VISION
Faithful Itanium-ABI **multiple + virtual inheritance** for madc (full Tier A layout +
Tier B polymorphic codegen), lowered to portable C11, interoperating with real
libstdc++. Driver: the retire-std-hardcoding campaign needs `std::string`/streams to be
**real header-defined classes**, which needs a faithful class model. User chose the
FULL build ("we're going to need it anyways for proper C++ support… do it right").
ONE model — the Itanium scheme REPLACES madc's old simplified single-vptr scheme.

## BRANCH TOPOLOGY (no drift)
- **`feature/multiple-inheritance-claude`** @ `3467ab1` — pushed (0/0), tracked tree clean.
  Cut from **`feature/retire-std-hardcoding-claude`** HEAD (so it carries the campaign's
  completed mangler + keystone — least drift). **22 commits** of MI work on top; **50
  ahead of develop**. develop UNTOUCHED. MIR fork pin unchanged (MI needs no fork work).
- When MI is done it **merges back into the campaign branch**; the stack promotes to
  develop together later.

## STATE: S1–S5 ALL DONE — MI FEATURE COMPLETE

S5 (RTTI / dynamic_cast / typeid) landed inline per
`docs/superpowers/plans/2026-06-03-multiple-inheritance-S5-rtti.md` (13 tasks, all gated:
build 0-warn, unit 55/55, integration 468, SMAUG boots, no-std gate 468; 6 `tests/test_rtti_*`
match g++). Summary: S5a vtable gains the Itanium 2-word prologue `[offset_to_top, &_ZTI]` per
address point; S5b `class_typeinfo_def` emits ABI-faithful `_ZTS`/`_ZTI` (__class/__si/__vmi)
referencing the real libsupc++ type_info vtables; S5c `dynamic_cast<T*>` → `__dynamic_cast`;
S5d `<typeinfo>` header + `typeid` (polymorphic reads `vptr[-1]`) + `typeid(x).name()`.
**NEXT: merge `feature/multiple-inheritance-claude` → `feature/retire-std-hardcoding-claude`;
then the std:: campaign resumes string-first.** Orthogonal gaps found (logged, NOT fixed):
virtual-dtor-only doesn't set has_vtable; const members / trailing-const methods unparsed;
`&reference-param` yields address-of-the-pointer. See memory `project_multiple_inheritance`.

---

## (historical) STATE: S1–S4 DONE (+ a crash fix), S5 is the last stage
Verified each stage: build 0-warn, unit `test_class_layout` (41 assertions), integration
**463 passed** (6 MI tests, each matched to g++; same known-6 fails + flaky
`testfortypedcomma`), **SMAUG boots** (port 4000), std-hardcoding gate **468** (MI added
ZERO — it was 469 at campaign HEAD, −1 from a reworded comment, held at 468 through S2–S4).
Single inheritance + non-virtual-base classes are **byte-identical** throughout (every
MI change is gated/no-op when there are no MI/vbases) — that's why SMAUG (single-inh) is
unaffected.

**What works now (all matching g++), MI tests `tests/test_mi_*.mad`:**
- **S1 layout engine** (`DataDefCLASS::compute_layout` in parser.cpp): Itanium sizes +
  per-base offsets + vptr placement; primary base @0 shares vptr, secondary bases own
  vptr at non-zero offset, virtual bases appended once at end (deduped). Unit-tested vs
  the g++/clang++ probe (`tests/unit/test_class_layout.cpp`). `BaseSpec`/`bases`,
  `vbase_offset`, `secondary_vptr_owners`, `nvsize`, `has_vptr_slot`, `base_offset_of`,
  `collect_vbases`, `apply_member_layout`, `build_vtable_groups`, `find_vslot` all on
  DataDefCLASS (include/datadef.h).
- **S2 live layout**: parse `virtual`/comma base lists; origin-tagged flatten over all
  bases; `compute_layout` LIVE (replaced the +8 vptr fix-up); offset-ordered C11 struct
  emission (`__pad`/secondary `__vptr_<off>` fields so c2mir reproduces the layout);
  compile-time `this`-adjust on base-method calls; vptr-for-vbase (a vbase class carries
  a vptr even with no virtual methods). `test_mi_parse` 1 2 3 9 4; `test_mi_layout` MIc
  24 / vbase-D 24; `test_mi_method` 100 200.
- **S3 virtual dispatch**: MI ctor/dtor chaining over all bases (offset-adjusted);
  grouped vtables (primary + one group per secondary polymorphic base, address points);
  secondary vptr init; dispatch via `find_vslot` (owning group's vptr field + in-group
  slot); upcast offset-adjust; `is_or_derives_from` walks full graph; this-adjusting
  **thunks** (`Cls__thunk_<off>_<m>`) for overrides reached through a secondary base.
  `test_mi_ctor_order` A+B+C+C-B-A-; `test_mi_vdispatch` 10 20 10 20 105.
- **S3 crash fix**: a polymorphic STACK object with NO user ctor never had its vptr set
  → base-ptr dispatch crashed (single-inh too). `class_ctor_call` now emits the grouped
  vptr-init for ctorless `has_vtable` classes (the implicit default ctor's job).
- **S4 virtual-base construct/destruct ONCE**: complete-vs-base-object split, no
  ctor-ABI change. Construction (site-based): ctors construct only non-virtual bases;
  the complete site (`class_ctor_call`/`new`) constructs transitive-deduped vbases first
  (`vbase_ctor_stmts`). Destruction (complete-dtor split): `Cls___dtor` = base dtor
  (skips vbases); `synth_complete_dtor_def` emits `Cls___dtor_complete` (base dtor +
  vbase dtors reverse, once); cleanup/delete/unwind/try sites target
  `class_complete_dtor_symbol`. `test_mi_vbase_ctor` T+L+R+D+D-R-L-T-.

## KEY CODE ANCHORS
- `include/datadef.h`: DataDefCLASS (`BaseSpec`, `bases`, `vbase_offset`,
  `secondary_vptr_owners`, `nvsize`, `own_block_off`, `has_vptr_slot`, `member_origin`,
  `VtableGroup`/`vtable_groups`/`find_vslot`, `is_or_derives_from` graph walk).
- `src/parser.cpp`: `compute_layout`, `apply_member_layout`, `collect_vbases`,
  `base_offset_of`, `build_vtable_groups` (after findMethod ~3716+); base-list parse +
  origin-tagged flatten + the `compute_layout()`/`apply_member_layout()`/
  `build_vtable_groups()` finalize call in TokenCLASS::parse.
- `src/cir_builder.cpp`: `class_struct_def` (offset-ordered emission); `class_vtable_def`
  (grouped sub-tables + thunks, out-param); dispatch (`find_vslot`, ~3095); vptr init
  (ctor prologue ~7043 + `new` ~4573); `class_ctor_call` (vbase construct + ctorless
  vptr-init); ctor/dtor chaining (~7136/7173, skip vbases); `synth_dtor_def`,
  `synth_complete_dtor_def`, `class_complete_dtor_symbol`, `vbase_ctor_stmts`,
  `vbase_dtor_stmts`; Pass 1.5 vtable emit + Pass 1.6 synth-dtor + Pass 1.7 complete-dtor
  (~7577/7807/7817).

## NEXT — S5 (the last MI stage): RTTI / dynamic_cast / typeid
Per spec §9: emit Itanium `type_info` objects (`__class_type_info`/`__si_class_type_info`/
`__vmi_class_type_info`) as C structs referencing libsupc++ type_info vtables, named
`_ZTI<class>`(+`_ZTS<class>`), pointed to from each vtable's RTTI slot;
`dynamic_cast<T>` → `__dynamic_cast`; `typeid` → the type_info. std:: classes REFERENCE
libstdc++'s type_info (mangler), emit nothing; only USER classes emit it. No S5 plan
written yet — write it (writing-plans) when starting; it gets its own bite-sized plan.
The mangler currently has NO `_ZTI/_ZTS/_ZTV/_ZTT/_ZTh` support (add it for S5 — but for
USER-class dispatch madc uses its own `Cls__vtable` naming, so real `_ZTV` is only needed
for libstdc++ interop = campaign).

## DEFERRED (logged, no current test/SMAUG/campaign consumer — own follow-ons)
- **Construction vtables / VTT** — a base ctor making a *virtual call during
  construction* seeing the under-construction vtable. (S4 builds vbases once but doesn't
  install construction vtables.)
- **Runtime `vbase_offset`** — dispatching a virtual method *declared in a virtual base*
  through a base pointer whose most-derived type is unknown (today the static-offset path
  via `base_offset_of`/`vbase_offset` covers the statically-typed case).

## RELATIONSHIP TO THE std:: CAMPAIGN (the "why are there still hardcoded string helpers" answer)
The retire-std-hardcoding campaign is NOT finished — gate **468** (target 0). cir_builder.cpp
still has ~79 Layer-3 builtins: `string_construct/destruct/assign/cstr/concat/obj_arg`,
`STR_CTOR/STR_DTOR`, `ddSTRING`, `is_std_string`, `sstream_*`, `__std_cout/cin/cerr/clog/
endl/to_string`. These are ANCIENT (predate the campaign; on develop/master too), the
campaign's **unstarted deletion phase** (inc 6/7) — NOT a regression, NOT snuck back
(gate moved only 469→468, never up). The campaign built the **keystone** (a bodyless
std:: method binds to the real libstdc++ symbol via the mangler — proven on a couple
fstream methods) but deferred the bulk migration. **std::string is the big one (~239
sites)** and was reordered to **string-first, AFTER MI**, because making it a real
header-defined class needs the faithful class model S1–S4 just built. The std:: campaign
does NOT need S3–S5 (libstdc++ owns std:: vtables; S1+S2 + mangled calls + `base_offset_of`
already cover the stream `good()`/vbase-offset case); S3–S5 are proper-C++ USER-class work.

**Two resume options from here:** (a) finish MI **S5** (RTTI), then merge MI → campaign
branch, then campaign string-first; or (b) MI is far enough (S1–S4 give the class model
std::string needs) to **pause MI and pivot now to the campaign string-first** deletion
(migrate std::string → header class via the keystone, delete the Layer-3 helpers, gate→0),
returning to S5 later. Either is valid; user's call.

## VERIFICATION COMMANDS (cap heavy runs; one at a time)
```
( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )    # 0
( ulimit -t 60; timeout 60 ./bin/test_class_layout | tail -2 )                    # 41 assertions pass
( ulimit -t 700; timeout 800 bash scripts/run_tests.sh > tmp/g.log 2>&1 ); tail -1 tmp/g.log   # 463 passed + known-6 + flaky
bash scripts/check-no-std-hardcoding.sh | head -1                                 # 468 (MI track adds none)
# MI tests:
for t in test_mi_parse test_mi_layout test_mi_method test_mi_ctor_order test_mi_vdispatch test_mi_vbase_ctor; do ( ulimit -t 60; ./bin/madc tests/$t.mad 2>&1 | grep -v setrlimit | tail -1 ); done
# SMAUG soak (real port; 124 = survived):
cd /workspace/MadSMAUG/runtime/area; timeout 50 /workspace/madc/bin/madc /workspace/MadSMAUG/src/SMAUG.mad 4000 > /workspace/madc/tmp/s.log 2>&1; echo $?; pkill -9 -f 'bin/madc'; grep -c "ready at" /workspace/madc/tmp/s.log
```

## METHODOLOGY (enforced)
gcc/g++/clang IS canon — probe layout/symbols (`-fdump-record-layouts`, `c++filt`,
`nm -D`) before asserting. No shortcuts / fix the deepest layer. Gate EVERY change: build
0-warn → run_tests holds the baseline (diff the FAIL list) → SMAUG soak for any
parser/codegen change → commit → push. Never leave a crashing/wrong bug "logged" — fix it
(the S3 stack-vptr crash was fixed immediately on request). Commit msgs: no embedded `"`.
GOTCHA: a compile error shows under `grep -icE "warning:|error:"` — don't misread the
count as warnings; a stale binary then masks it. After an edit, confirm the rebuild took.
GOTCHA: `FuncDef::name` is empty — use the Variable's `mv->name` for a method symbol.
