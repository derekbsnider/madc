# Builtin std::string Machinery — Migration Inventory

Read-only recon for the "retire all std:: hardcoding" campaign. Goal: replace
the BUILTIN std::string (the `ddSTRING` DataDef, `add_string_methods`, the
`STR_*` statics, the `string_*` extern-C wrappers, every CIR special-case
branch) with the generic header-defined-class path that `include/madc/string`
already begins (the "keystone": bodyless std:: methods mangle to real libstdc++
symbols). Finish-line gate: `scripts/check-no-std-hardcoding.sh` greps
`src/`+`include/` for this machinery and its count must reach 0.

Scope searched: `src/` and `include/` only (not docs/, not tests/).

---

## Pattern totals (reconciliation)

| Pattern                                              | Count |
|------------------------------------------------------|------:|
| `ddSTRING` (incl. `DataDefSTRING`/`ddSTRINGref`)     |   231 |
| `is_std_string` / `is_std_string_ref` / `is_std_string_value` |  48 |
| `is_string_object_value` / `is_string_returning_call` / `is_string_object` / `is_string_operator_plus` | 38 |
| `string_*` extern-C wrappers (src/ refs)             |   106 |
| `STR_*` statics (src/cir_builder.cpp)                |    47 |
| `string_obj_words`                                   |     3 |
| `sizeof(std::string)` / `sizeof(std::string &)`      |    10 |

`ddSTRING` by file: parser.cpp 109, cir_builder.cpp 19, ns_php.cpp 23,
ns_perl.cpp 19, ns_rust.cpp 18, ns_python.cpp 15, ns_ruby.cpp 10, ns_js.cpp 6,
include/datadef.h 4, include/tokens.h 3, include/madc.h 2, madc_program.cpp 1,
include/datatokens.h 1. (Total 230 src+include + 1 in this header listing path.)

NOTE: many `ns_*.cpp` `ddSTRING` hits and the `is_string()` calls in `ns_*.cpp`
are the **borrowed-language namespace function signatures** (php::/perl:: take
`&ddSTRING` params) and the **`MadValue::is_string()`** PHP-array discriminator
— a *different* `is_string()` (MadValueKind, datadef.h:970). Those are NOT
string-builtin internals; they are ordinary uses of `string` as a parameter
type and will follow the type automatically once `ddSTRING` is replaced by the
header-class DataDef. They are listed under cluster H (Consumers) for
completeness but require no bespoke migration.

---

## Cluster A — Type registration / identity (the root)

**Role:** defines the builtin `ddSTRING` / `ddSTRINGref` DataDef singletons,
the `is_string_class()` virtual + free recognizers, and registers `string` as a
std:: type. Everything else keys off these.

References & sites:
- `include/datadef.h:838-839` — `class DataDefSTRING` / `class DataDefSTRINGref`
  (DDClass subclasses; ctor passes `sizeof(std::string)` / `sizeof(std::string&)`;
  override `is_string_class()`→true).
- `include/datadef.h:124-131` — base `DataDef::is_string_class()`/`is_string()`
  virtuals (default false).
- `include/datadef.h:864` — `DataDefPTR::is_string_class()` propagates identity
  through `string*`/`string&` base_type.
- `include/datadef.h:1103-1104` — `extern DataDefSTRING ddSTRING; extern DataDefSTRINGref ddSTRINGref;`
- `include/datadef.h:1107-1141` — the three free recognizers `is_std_string`,
  `is_std_string_ref`, `is_std_string_value`.
- `src/parser.cpp:3554-3555` — the actual definitions `DataDefSTRING ddSTRING; DataDefSTRINGref ddSTRINGref;`
- `src/parser.cpp:5680-5681` — `std_types["string"] = make_namespace_type_token("string", ddSTRING);`
  (registers `std::string` to the builtin DataDef when `_include_string`).
- `src/parser.cpp:12953-12955` and `12994-12996` — `&td->definition == &ddSTRING`
  contextual-identifier checks (treat the `string` type keyword as a valid C
  identifier in `is_contextual_identifier_token` / `contextual_identifier_name`).
- `include/tokens.h:990` — `TokenString::setDataType` guards on `d->is_string()`.
- `include/madc.h:519` — `o.type->is_string()` branch.

**What it does:** establishes the builtin type object and the
"is this the string type?" predicate (`is_std_string` → `is_string_class()`
virtual). `make_namespace_type_token` binds the spelling `string` to `&ddSTRING`.

**Migration note:** the header-defined `std::__cxx11::basic_string<...>` class
(parsed from `include/madc/string`) becomes the canonical DataDefCLASS for
`string`; `std_types["string"]` must resolve to *that* class (likely via the
`string` typedef the header will add, per its STAGING comment). The
`is_std_string()` recognizer stays useful but must switch from the
`is_string_class()` tag to a **generic class-identity test** (compare against
the header-class DataDef pointer, or a "this is the std::string instantiation"
predicate). **Needs-mechanism-first:** the header class must be fully populated
(all operators/methods, the `string` typedef) and `std_types["string"]` repointed
*before* `ddSTRING` can be deleted. This cluster migrates LAST (it is the anchor
all other clusters resolve through), but the *identity predicate* indirection
(`is_std_string` returning true for the header class) should land EARLY so
downstream clusters can be migrated against a single recognizer.

---

## Cluster B — Method/operator/ctor registration (`add_string_methods`)

**Role:** populates `ddSTRING.methods`/`.ctors`/`.method_map` with ~30 entries,
each binding `emit_symbol` to a mangled libstdc++ symbol or an extern-C wrapper.

References & sites:
- `include/madc.h:1177` — decl `void add_string_methods();`
- `src/parser.cpp:5879` — the single call site (`_parser_init`, gated on string include).
- `src/parser.cpp:4050-4293` — the whole `add_string_methods()` body. Sub-roles:
  - c_str/length/size/empty/clear/assign/append → `itanium_mangle_member_sub`
    (4063-4108, 4227-4243).
  - operator= / += (str + cstr overloads) → `itanium_mangle_operator_sub`
    (4116-4142).
  - operator+ (str + cstr) → emit_symbol `"string_concat"` extern-C (4154-4164).
  - operator[] → mangled non-const `ixEm`, returns_ref (4172-4177).
  - operator== / != → emit_symbol `"string_equals"` (4186-4200).
  - operator< / > / <= / >= → emit_symbol `string_lt/gt/le/ge` (4210-4222).
  - ctors string()/string(const char*)/string(const string&) → mangled
    `C1` symbols (4252-4283); dtor `~string` → mangled `D1` (4286-4290).
- `src/madc.h:103`, `src/cir_builder.cpp:7839` — comments referencing the
  `add_string_methods` population invariant.

**What it does:** turns `ddSTRING` into a fully-populated class whose members
dispatch through `emit_symbol`. This is the bridge between "builtin" and "real
libstdc++": most members ALREADY use mangled symbols, mirroring the keystone
exactly. The only non-mangled members are operator+ (`string_concat`),
operator== / != (`string_equals`), and relational ops (`string_lt`..`string_ge`)
— libstdc++ exposes these only as inlined weak template symbols, so they cannot
be dlsym-resolved and stay as extern-C wrappers (cluster G).

**Migration note:** `include/madc/string` already declares ctor/dtor/length/
size/c_str/empty/clear/at/append/operator[]/operator=/operator+= bodylessly →
the keystone mangles them from the declaration. The migration **deletes
`add_string_methods` entirely** once the header declares the full surface; the
parser's `bind_std_libstdcpp_symbol` (per memory notes) auto-binds emit_symbol
from the parsed declaration. **Difficulty:** mostly mechanical (delete +
ensure header parity), EXCEPT the non-dlsym-able operators (operator+, ==, !=,
relational) which have no exported libstdc++ symbol — those must keep routing to
the extern-C wrappers. The header can declare them as **non-member operators**
bound (via the keystone hook or an explicit `emit_symbol`-style annotation) to
`string_concat`/`string_equals`/`string_lt`… The `<map>`/`<set>` headers
already do exactly this for `operator==`→`string_equals` (see
`include/madc/map:10`, `include/madc/set:10`), so the mechanism exists.
This cluster needs cluster A's header class first.

---

## Cluster C — `STR_*` mangled-symbol statics (cir_builder.cpp)

**Role:** a parallel copy of the mangled symbols, computed once as file-scope
statics, used by the CIR lowering for ctor/dtor/method emission.

References & sites:
- `src/cir_builder.cpp:779-803` — `STR_TYPE`, `STR_CTOR0_s`/`STR_CTOR_S_s`/
  `STR_CTOR_CP_s`/`STR_DTOR_s`/`STR_CSTR_s`/`STR_SIZE_s`/`STR_LENGTH_s`/
  `STR_CLEAR_s`/`STR_ASGN_S_s`/`STR_ASGN_C_s`/`STR_APP_S_s`/`STR_APP_C_s`
  (the `_s` std::string + the `.c_str()` `const char*const` aliases).
- Consumers: 813 (`STR_DTOR` in storage decl), 902/907/911 (`STR_CTOR_*`),
  928/936 (`STR_CSTR`), 1057/1059/1065 (`STR_DTOR` cleanup attr), 3232/3235
  (`STR_SIZE`), 5003/5009/5013 (`STR_CTOR_*`), 6181/6186 (`STR_CTOR_CP`),
  7775/7777/7778 (`STR_CTOR_S`).
- Comment cross-ref at parser.cpp:4250-4251 and 4060-4061 (they are
  deliberately duplicated between parser and cir_builder).

**What it does:** the CIR builder, when it lowers a string storage/ctor/dtor/
method directly (NOT through the registered FuncDef method path), splices these
mangled symbols into `N_CALL`/`cleanup` nodes.

**Migration note:** this is a **duplicated symbol source** — a P1 design
violation (the mangler should be the single source). Once the generic
class path drives all string lowering through the registered FuncDef
`emit_symbol`s (cluster A/B), these statics and their consumers must be
**replaced by reading the symbol off the resolved class member's FuncDef**, the
same way a user class's ctor/dtor/method symbols are obtained. **Difficulty:**
needs-mechanism-first — requires the CIR string ctor/dtor/method lowering
(cluster E) to route through the generic class-member lookup rather than the
hardcoded `STR_*` constants. After cluster E migrates, deleting these statics is
mechanical.

---

## Cluster D — Layout / sizeof

**Role:** sizes the opaque string storage buffer.

References & sites:
- `src/cir_builder.h:253` — `size_t string_obj_words() const;` decl.
- `src/cir_builder.cpp:581-588` — `string_obj_words()` = `ceil(sizeof(std::string)/sizeof(long))`.
- `src/cir_builder.cpp:594-599` — `object_class_words(cdd)` = generic version
  using `cdd->size` (already the header-class path).
- `include/datadef.h:838-839` — `DataDefSTRING`/`ref` ctors call `sizeof(std::string)`.
- `src/cir_builder.cpp:813` — `string_storage_decl` calls `string_obj_words()`.
- (out of scope but noted) `include/madcdat/mapper.h:396` — unrelated madcdat use.

**What it does:** computes buffer words from the compiler's own
`sizeof(std::string)`. This is the hardcoded-layout the campaign forbids: the
size must come from the **parsed header struct layout** instead.

**Migration note:** `include/madc/string` already lays out the 32-byte basic_string
(char* + size_type + 16-byte SSO) so madc's generic struct-layout computes the
size; a doctest cross-checks it against the real `<string>`. `object_class_words`
already derives words from `cdd->size`. Migration: **delete `string_obj_words`**,
route `string_storage_decl` through `object_class_words(header_class)`.
**Difficulty:** mechanical once the header class is the canonical DataDef
(cluster A); `object_class_words` is the drop-in replacement and already exists.

---

## Cluster E — CIR lowering branches (cir_builder.cpp string special-cases)

**Role:** the CIR builder's per-value string handling — storage decl, ctor
injection, argument materialization, return-by-value, member-wise copy. The
largest behavioral cluster.

Predicate helpers (the routing gates):
- `src/cir_builder.h:190,194,198,204` — decls of `is_string_object`,
  `is_string_object_value`, `is_string_operator_plus`, `is_string_returning_call`.
- `src/cir_builder.cpp:443-448` `is_string_object`; `450-491`
  `is_string_object_value`; `502-518` `is_string_operator_plus`; `539-555`
  `is_string_returning_call` (+ helper `call_target_funcdef` 529-537).
- `src/cir_builder.cpp:346,372` — `as_user_class` excludes / `as_object_class`
  canonicalizes to `&ddSTRING` (351-374); `as_class_instance` (378-382).

Lowering helpers and call sites:
- `string_storage_decl` 811-813; `string_ctor_call` 894-911; `string_cstr_arg`
  922-936/926; `string_obj_arg` 948-...; obj_storage_decl cleanup 1046-1076.
- `string_obj_arg` call sites: 984-985, 1008, 1765/1776/1778, 1840, 3257, 3343,
  3978, 4246-4247, 4278-4279, 4300/4303, 4339, 4500, 5002, 5040, 5125, 6185.
- `is_std_string`-gated branches: 1007, 1186, 1457, 3159, 3254, 3342, 3483,
  3905, 3977, 4129, 4200, 4301, 4338, 4497, 4689, 5039, 5124, 6331, 6679, 6768,
  6836, 7444, 7765.
- member ctor/dtor/copy symbols 3410-3412 (`MEMBER_CTOR_SYM="string_construct"`,
  `MEMBER_DTOR_SYM="string_destruct"`, `MEMBER_COPY_SYM="string_construct_copy"`);
  used at 3494-3507, 3796, 7196, 7333.
- new/alloc string path 4986-5013 (DataDefSTRING IS-A DataDefCLASS comment).
- string-element foreach fill 6679/6768-6777/6836-6845 (`string_assign`).
- throw c_str path 6331-6334.

**What it does:** routes any value the front end recognizes as a string object
(declared var, string& param, operator+ rvalue, retbuf-returning call, container
element) into the opaque-buffer + placement-ctor/cleanup-dtor lowering, and
coerces string objects↔const char* at call boundaries.

**Migration note:** the *mechanisms* here (opaque storage, cleanup-dtor,
copy-construct-into-retbuf, member-wise copy-assign) are exactly what the GENERIC
object-class path already does (`object_class_words`, `as_object_class`,
`class_copy_*`). The migration folds the string-specific helpers into the generic
class helpers: `string_storage_decl`→`obj_storage_decl(words(header_class))`,
`string_ctor_call`→generic `class_ctor_call`, the `STR_*`/`MEMBER_*_SYM`
constants→class-member FuncDef lookups (cluster C). The predicates
`is_string_object_value`/`is_string_returning_call`/`is_string_operator_plus`
stay (they answer "is this expression a string value?"), but rebased on the
header-class identity. **Difficulty:** mixed — most call sites are mechanical
re-routes to the generic helpers, BUT the operator dispatch (operator+ temp
materialization, ==/relational, []) and the const-char*↔object coercion need the
generic class operator/conversion machinery to handle them; see clusters F & G.
**Largest cluster; migrate in sub-steps test-by-test.**

---

## Cluster F — Operator handling (cir_builder class_operator/subscript)

**Role:** the CIR builder's per-operator lowering for string operands:
`+`, `=`, `+=`, `==`/`!=`, `<`/`>`/`<=`/`>=`, `[]`, and stream `<<`/`>>`.

References & sites:
- `src/cir_builder.cpp:507-517` — `is_string_operator_plus` (mirrors the tkAdd
  guard in `class_operator_call`).
- `src/cir_builder.cpp:4118-4129` — `rhs_is_string`/`p1_is_string` overload
  selection in operator lowering.
- `src/cir_builder.cpp:4195-4339` — the operator lowering block: operator=
  pointer-vs-string guard (4195-4200), ==/!= → `string_equals` (+ negate)
  (4235-4247), operator+ → `string_concat` out-slot temp (4264-4280),
  `+=`/method dispatch (4296-4339).
- `src/cir_builder.cpp:4497-4500` — operator[] subscript → `string_obj_arg(index)`.
- (parser side, the registration, is cluster B 4116-4222.)

**What it does:** selects the string operator overload by RHS type and emits the
mangled member call (=, +=, []) or the extern-C wrapper call (+ , ==, relational),
including the by-value operator+ temp-allocation + cleanup.

**Migration note:** the generic class-operator path
(`class_operator_call`/`class_subscript_call`) already drives user-class
operators by looking up the operator member and emitting its `emit_symbol`. The
migration makes string operators flow through that generic lookup once they are
registered on the header class (cluster B). **Needs-mechanism-first:**
**overload selection by argument type** (str-vs-cstr overloads) must be done by
the generic operator-resolution, not the hardcoded `p1_is_string`/`rhs_is_string`
branches; and **non-member operator resolution** (operator+/==/relational are
free functions in real C++, bound to the extern-C wrappers) must be supported
the way `<map>`/`<set>` already declare `operator==`→`string_equals`. Once those
two mechanisms resolve string operators generically, the hardcoded branches
delete. This is the **most mechanism-heavy cluster** — flag it as a blocker.

---

## Cluster G — Runtime wrappers (extern-C definitions)

**Role:** the real C++ implementations the emitted C calls into.

References & sites:
- `src/madc_mir_backend.cpp:58-110` — `string_construct`, `string_destruct`,
  `string_construct_cstr`, `string_construct_copy`, `string_assign`,
  `string_assign_cstr`, `string_cstr`, `string_length`, `string_append`,
  `string_append_cstr`, `string_clear`, `string_equals`, `string_lt/gt/le/ge`,
  `string_concat`.
- `src/madc_mir_backend.cpp:170-173` — `streamin_string`/`streamin_getline`
  (string-object stream helpers).
- `src/parser.cpp:4047,4409` — `madc_string_length` declaration/definition (the
  legacy-backend placeholder fn pointer used by the parser registrations).

**What it does:** placement-new / dtor / assign / compare / concat over real
`std::string` objects; the dlsym-resolved targets of the non-mangled emit_symbols.

**Migration note:** PARTIALLY PERMANENT. The members that ALREADY mangle to real
libstdc++ (`construct`→ctor C1, `destruct`→dtor D1, `length`/`size`/`c_str`/
`assign`/`append`/`clear`/operator=/operator+=/operator[]) make their
`string_*` wrapper redundant once the keystone binds the mangled symbol — those
wrappers (`string_construct`, `string_construct_cstr`, `string_construct_copy`,
`string_destruct`, `string_assign`, `string_assign_cstr`, `string_cstr`,
`string_length`, `string_append`, `string_append_cstr`, `string_clear`) can be
**deleted**. The non-dlsym-able operators (`string_equals`, `string_lt/gt/le/ge`,
`string_concat`) have NO exported libstdc++ symbol (inlined weak templates) and
must **stay** as the bound targets of the header's non-member operators (same as
`<map>`/`<set>` already do). **Difficulty:** mechanical deletion of the
redundant wrappers after clusters B/E migrate; the surviving comparison/concat
wrappers are a permitted runtime, not std-hardcoding — confirm the finish-line
gate `scripts/check-no-std-hardcoding.sh` treats `string_equals`/`string_concat`
as runtime, not as builtin machinery to flag.

---

## Cluster H — Consumers / coercion (follow-the-type, mostly automatic)

**Role:** code that consumes `string` as a value/parameter type or coerces
string↔const char* — it uses the *type*, not the *builtin*, so it migrates for
free once cluster A repoints the DataDef.

References & sites:
- Coercion: `src/parser.cpp:310,7790,8671` and `8635-8672` (ternary char*/string
  unification, `dd->is_string()`); `src/cir_builder.cpp:1694-1696`,
  `1850`, `6003-6006` (`string_cstr_arg`, char* PKc binding); `madc_program.cpp:399,
  1918,2046,2107,2892,4053` (`is_std_string`/`is_string` coercion in the
  program/emit-c path).
- Param handling: `src/parser.cpp:6042,15111-15140,15185` (`&ddSTRINGref` scope
  param, ref-param decisions).
- Stream registration using `&ddSTRING`: parser.cpp:5397/5409/5417 (fstream open),
  5432/5433 (printstr), 5478-5494 (getline/to_string/stoi/system/getenv/dlopen).
- Borrowed-language namespace signatures: ns_php.cpp (23), ns_perl.cpp (19),
  ns_rust.cpp (18), ns_python.cpp (15), ns_ruby.cpp (10), ns_js.cpp (6) — all
  `&ddSTRING` param types in addFunction signatures (and the PHP/borrowed std::
  table at parser.cpp:5738-5868).
- `MadValue::is_string()` (datadef.h:970, used across ns_*.cpp/madc_mir_backend.cpp:294
  /ns_common.cpp:116) — a SEPARATE `is_string()` (MadValueKind tag), NOT the
  DataDef string builtin; no migration needed.
- `include/datatokens.h:1`, `include/madc.h:519`, `include/tokens.h:990` — token
  helpers keying on the string type.

**What it does:** uses `&ddSTRING` as a parameter/return DataDef in function
signatures and coerces string→const char* at boundaries.

**Migration note:** every `&ddSTRING` reference becomes `&<header string class>`
(or the resolved `string` typedef DataDef). If cluster A keeps a stable
`ddSTRING`-named handle pointing at the header class during transition, these
sites need NO edits; otherwise they are a **mechanical rename**. The
`MadValue::is_string()` family is unrelated and stays. **Difficulty:** mechanical
/ automatic. Migrate alongside or after cluster A's repoint.

---

## Dependencies / ordering

```
A (identity + header class repoint)  ── anchor; predicate-indirection lands EARLY,
   │                                     full repoint + ddSTRING deletion lands LAST
   ├──> B (add_string_methods → header-declared surface)   [needs A's header class]
   │       │
   │       ├──> C (STR_* statics → class-member symbol lookup)  [needs B]
   │       └──> F (operators via generic resolution)            [needs B + new mechanisms]
   │
   ├──> D (string_obj_words → object_class_words)          [needs A; replacement exists]
   ├──> E (CIR lowering → generic object-class helpers)    [needs A,B,C,D,F]
   ├──> G (runtime wrappers: delete redundant, keep ==/concat) [needs B,E]
   └──> H (consumers / coercion: follow the type)          [auto if A keeps a handle]
```

## Suggested migration ORDER (lowest-coupling / mechanical first)

1. **D — Layout/sizeof.** `object_class_words(header_class)` already exists; once
   the header class is parsed, swap `string_obj_words` → it. Low risk, isolated.
2. **A (early half) — identity predicate indirection.** Make `is_std_string()`
   recognize the header-parsed `std::__cxx11::basic_string<char,...>` class (by
   class-identity), so all downstream clusters can be migrated against one stable
   recognizer while `ddSTRING` still exists.
3. **G (partial) — confirm survivors.** Verify `string_equals`/`string_lt..ge`/
   `string_concat` are gate-clean as runtime, and that `<map>`/`<set>`-style
   non-member operator binding is reusable for `<string>`.
4. **B — method/ctor/dtor surface to the header.** Move the mangled-member and
   ctor/dtor registrations into `include/madc/string` (most already match the
   keystone); add the `string` typedef; bind the non-member ==/relational/+ to
   the extern-C wrappers via the header (as map/set do).
5. **C — delete STR_* statics.** Route CIR ctor/dtor/method lowering through the
   resolved class-member FuncDef `emit_symbol`s.
6. **F — operators via generic resolution.** Move operator dispatch off the
   hardcoded `p1_is_string`/`rhs_is_string` branches onto generic class/non-member
   operator resolution. **MECHANISM-BLOCKED** (see below).
7. **E — fold CIR string lowering into the generic object-class path.** Largest;
   do test-by-test. Replace string-specific helpers with `obj_storage_decl` /
   `class_ctor_call` / `class_copy_*`.
8. **G (finish) — delete redundant runtime wrappers** (construct/destruct/assign/
   append/cstr/length/clear) now that the keystone binds the mangled symbols.
9. **A (final half) + H — repoint `std_types["string"]` to the header class,
   delete `DataDefSTRING`/`ddSTRING`/`add_string_methods`, rename remaining
   `&ddSTRING` consumers.** Run `scripts/check-no-std-hardcoding.sh` → must be 0.

## Mechanism-blocked clusters (need new machinery before they can migrate)

- **F (operators)** — requires (a) **overload selection by argument type** in the
  generic operator-resolution (to pick the str-vs-cstr operator=/+=/+ overload),
  and (b) **non-member operator resolution** for operator+/==/!=/relational (free
  functions bound to extern-C wrappers). Mechanism (b) partially exists
  (`<map>`/`<set>` bind `operator==`→`string_equals`); (a) must be generalized
  from the current hardcoded RHS-type branches.
- **B (the non-dlsym operators within it)** — depends on F's non-member binding
  mechanism to declare operator+/==/relational in the header.
- **C / E** — not mechanism-blocked per se, but **ordering-blocked**: they cannot
  delete the `STR_*`/string-specific helpers until B (registration) and F
  (operator resolution) provide the generic equivalents the lowering will call.

All other clusters (A-identity-predicate, D, G-deletions, H) are mechanical and
can proceed without new mechanism.
