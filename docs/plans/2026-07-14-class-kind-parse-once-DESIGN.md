# Class-KIND Parse-Once Design

**Status:** design complete; no implementation is part of this sitting.
**Date:** 2026-07-14.
**Branch:** `feature/class-kind-parse-once-design-codex`.
**Parent charter:**
[`2026-07-13-instantiate-bucket-plan.md`](2026-07-13-instantiate-bucket-plan.md),
Slice B, and
[`2026-07-14-CODEX-HANDOFF-forest-perf-instantiate-cirbuild.md`](2026-07-14-CODEX-HANDOFF-forest-perf-instantiate-cirbuild.md).

## 1. Decision

Class-template instantiation will gain a class declaration KIND on the existing
parse-once spine:

1. Parse each primary or partial class-template definition once, with typed
   `DataDefTemplateParam` placeholders.
2. Normalize the parse result into an immutable `ClassPattern`. The pattern
   stores structural type expressions and source-ordered declarations, not
   concrete offsets or parser-global names.
3. Select partial specializations and bind arguments through the current
   `instantiate_template_use` front end.
4. Instantiate an eligible pattern by substituting its declaration tree,
   registering the concrete class, and running the existing layout and symbol
   completion machinery. Do not inject class tokens or call
   `TokenSTRUCT::parse` / `TokenCLASS::parse`.
5. Decide eligibility before an instantiation starts. An ineligible pattern
   takes today's token-parser path as its only path and is counted with a stable
   `[why:]` reason. An eligible pattern either completes through substitution or
   raises a loud internal error after rolling back its transaction. It never
   retries through the parser.

The forest must store the normalized pattern as parsed state. A bound consumer
must not parse the saved class body to recreate it.

This is a front-end change, not a forest-only optimization. Both live and bound
compiles must move in the same direction.

## 2. Measured reason for the work

The instantiate-bucket profile attributed 1.81 billion of 3.465 billion
instructions (about 52%) to class-body re-parsing. In the measured bound
`testsubscript` compile, 251 `parseKeyword` class-body calls were driven by only
seven consumer specializations. Method bodies are already deferred; the
repeated work is primarily the class shell, member declarations, registration,
and completion.

Mission 1 removed the unrelated eager RECORDS inverse-transform cost. Its final
bound `cir build` median is 0.270 s and packed wall is 0.593 s. Slice B attacks
the remaining parser-instantiation bucket and must not alter the compression
format or binary-size tradeoff merely to improve timing.

Use the binaries the build already owns:

- `bin/madc`: ordinary non-stripped development binary.
- `bin/madc-release`: stripped release binary with the embedded forest.
- `MADC_CPU_LIMIT=<seconds>`: the program's CPU-cap control.

Do not create an extra scratch optimized executable for this work.

## 3. Current control flow and retained parts

Today `TokenTEMPLATE::parse` captures a `TemplateDef::body` token vector without
parsing the class. `Program::instantiate_template_use` then:

1. Parses explicit arguments and defaults.
2. Builds type, template-template, pack, and non-type substitution maps.
3. Selects a partial specialization and incorporates its deductions.
4. Computes the canonical C++ spelling and internal registration key.
5. Returns an existing `datatype_map` specialization on a cache hit.
6. Creates an opaque/dependent shell when the arguments are not concrete.
7. Otherwise clones the selected body, substitutes tokens, renames the class,
   injects the result, isolates parser scope, and calls `parseKeyword`.
8. Attaches out-of-line member definitions, instantiates out-of-line nested
   classes, repairs alias keys, and verifies registration.

Steps 1 through 6 stay as the shared binding front end. Step 7 is the repeated
class parse this design replaces for eligible patterns. Step 8 remains, but its
out-of-line nested-class subpath must also be structural before a pattern using
that feature can be eligible.

The binding result should become an explicit value object, tentatively
`ClassTemplateBinding`, containing:

- selected `TemplateDef` and `ClassPattern` identity;
- scalar type arguments by template-parameter index;
- type-pack arguments by template-parameter index;
- non-type argument token/value bindings;
- template-template bindings;
- namespace and enclosing-class context;
- canonical C++ spelling, internal key, registered key, and any legacy alias
  key;
- the dependent-surface verdict and source provenance.

Both the legacy sole-parse lane and the pattern lane consume this same binding.
Argument deduction, defaults, partial-specialization ranking, cache identity,
and opaque-shell policy must not fork.

## 4. Parser side-effect inventory

This inventory is the output contract. The pattern instantiator must reproduce
every applicable item, or the pattern is not eligible yet.

### 4.1 Aggregate identity, registration, and scope

| Current producer | State produced or mutated | Required structural equivalent |
|---|---|---|
| Class/struct head parsing | Source tag, class/struct/union flavor, default access, `final`, typedef/definition-only mode | Store source flavor and normalized flags in the pattern; derive the concrete internal name from the binding |
| Qualified/nested/local class handling | `enclosing_class`, owner-qualified emitted name, `function_local_class_identities`, scoped source alias | Reuse the existing stable identity helpers; nested pattern nodes use parent-local ids, then bind to the concrete owner |
| Canonical-name setup | `canonical_cpp_spelling`, constructor source name, namespace spelling, `from_system_header`, `has_dependent_surface` | Set from the binding and pattern provenance before method symbols are minted |
| Forward completion | Reuses and completes an existing `DataDefCLASS`/`DataDefSTRUCT` object in place | Preserve pointer identity; allocate only when no compatible shell exists |
| Early registration | `struct_map`, `datatype_map`, `namespace_datatype_map`, enclosing `type_aliases`, scoped typedef aliases | One shared `register_aggregate_shell` helper used by parser and pattern lanes |
| Decl-index taps | `pack_tap_struct`, `pack_tap_type`, and relevant `pack_tap_name` entries | Fire once for concrete public names; never tap temporary pattern names |
| Self aliases | `type_aliases[class_source_name]` and canonical constructor spelling alias | Populate before member signature resolution |

`TokenCLASS::parse` deliberately does not create a class `TopDecl`; class
visibility rides the registered type graph. `TokenSTRUCT::parse` does create
`TopDecl` entries for bare/tagged definitions and combined typedef/variable
forms. Class-template definitions normally end in a bare semicolon, but the
struct output contract remains necessary for explicit specializations and
future source forms.

When completing an existing shell, preserve non-pattern runtime/binding state:
`extern_ctor`, `extern_dtor`, `_dtor_ptr`, `is_extern_template_instantiated`,
and an already assigned type id. Temporary pattern flags
`is_dependent_placeholder`/`opaque_concrete_tag` must not leak into a concrete
result. The currently unused `staticconst` vector must remain preserved or empty;
the new path must not silently give it a second meaning.

### 4.2 Aggregate storage and declaration data

The concrete aggregate must populate the complete `DataDefSTRUCT` parallel
state, not only `members`:

- `members`, including source typedef spelling and origin token;
- `member_counts`, `member_array_flags`, `member_dims`, and
  `member_count_exprs`;
- `member_offsets`;
- `member_bitfields`, including storage unit, bit offset/width, signedness, and
  scalar-storage order;
- `member_access`;
- `member_origin` and `member_vbase` for inherited members;
- `member_explicit_align`;
- `member_default_inits`;
- `anonymous_aggregates` and `has_anon_aggregate`;
- `runtime_size_expr`;
- `pack`, `max_align`, `tag_explicit_align`, `union_layout`,
  `reverse_scalar_storage`, `is_anonymous`, and `is_complete`;
- bit-field-run state at construction time, ending cleanly before completion.

The instantiator must call the existing `addMember`, `addBitField`,
`addUnnamedBitField`, `addAnonymousAggregate`, alignment, and finalization
helpers. It must not reproduce their layout arithmetic in a second
implementation.

For a source `struct`, concrete member types decide whether the result is
promoted to `DataDefCLASS`: an object member by value or an NSDMI makes it
non-trivial. A placeholder is not an object, so the pattern parse's temporary
classification is not authoritative. Re-run the existing promotion predicate
after substitution over concrete members.

### 4.3 Bases and inherited state

`TokenCLASS::parse` currently produces or mutates:

- ordered direct `BaseSpec` records with access and virtual flags;
- `base_class` as the first-base compatibility view;
- flattened non-virtual-base members, with every parallel member field copied;
- a deduplicated virtual-base closure and `member_vbase` ownership;
- inherited `method_map` entries under first-wins rules;
- inherited destructor and polymorphism flags;
- `virtual_methods`, `vtable_slots`, and `has_vtable`;
- concrete `BaseSpec::offset`/`is_primary`, `vbase_offset`,
  `secondary_vptr_owners`, `nvsize`, `own_block_off`, `class_align`, and
  `has_vptr_slot` during layout;
- final member offsets via `apply_member_layout`;
- grouped vtables via `build_vtable_groups`.

The pattern stores only source-order base type expressions, access, and virtual
flags. After substitution, the existing base-flattening and completion code is
called on concrete base classes. Pattern offsets, if any were observed during
capture, are discarded.

### 4.4 Aliases, nested types, and static data

Class-body parsing can produce:

- class-scope `typedef` and `using` entries in `DataDefCLASS::type_aliases`;
- nested class/struct/union definitions and forward declarations, including
  owner-qualified names and `enclosing_class` links;
- nested enum definitions and enum aliases;
- alias templates and nested class templates through `template_map`,
  `partial_spec_map`, or the member-template registration surfaces;
- imported base members from `using Base::member`;
- `static_member_types`;
- folded integral `static_member_const_values`;
- no global `user_typedef_names` or top-level typedef emission for class-scope
  aliases.

Nested types require two phases: pre-register all nested shells in source order,
then fill them. This permits self and sibling references without parsing. A
nested pattern is addressed by a pattern-local node id; no temporary parse name
is durable state.

### 4.5 Methods, constructors, destructors, and operators

Normal methods and special members currently involve all of the following:

- `funcdef_map` entry and a `Variable` in the Program variable store;
- `FuncDef::returns`, parameter types, hidden `__this`, ref/const flags,
  canonical parameter spellings, typedef spellings, defaults and raw default
  tokens, varargs/void-param flags, trailing return, and declaration file;
- a `Method` with parameter Variables and `owner_class`;
- access and `vfSTATIC` flags on the method Variable;
- source-ordered `DataDefCLASS::methods`;
- `method_map` display-name binding;
- `ctors`, `has_user_ctor`, `has_user_dtor`, and
  `has_deleted_copy_assign`;
- overload-unique internal symbols, including nullary operator disambiguation
  and same-arity/type overload ranks;
- `method_display_name`, `local_emit_name`, `emit_symbol`, and
  `storage_alias_name`;
- declaration-only/defaulted/deleted/pure-virtual/const-method state;
- Itanium direct binding through `bind_declared_cpp_symbol`;
- virtual override detection and vtable slot insertion;
- conversion-operator display names, which are also the forest restore key.

The pattern must store a parsed signature recipe, not a substituted source
string. Each method pattern carries:

- declaration ordinal and semantic kind: method, constructor, destructor,
  conversion, or operator;
- display/source name and operator kind;
- access, static, virtual, const/ref, and declaration flags;
- structural return and parameter `TypePattern`s;
- parameter names, defaults, typedef source names, and canonical-spelling
  ingredients;
- member-template parameter metadata when applicable;
- raw body/mem-initializer/trailing-return token runs only where the existing
  deferred-body machinery still consumes them.

Concrete signature construction must be factored into a shared helper used by
`parseFunction` completion and the pattern instantiator. The helper, not the
pattern walker, owns hidden-parameter insertion, overload symbols, Program
registration, method maps, and ABI binding.

### 4.6 Member templates

`register_skipped_class_template_function` currently creates a declaration-only
varargs placeholder and records:

- `is_member_template` and template parameter names/type/pack flags;
- `member_template_owner`;
- return and parameter spelling recipes;
- the retained `member_template_decl` for body-bearing templates;
- owner `methods`, `method_map`, and possibly `ctors`/`has_user_ctor`.

This is part of the container closure, not an optional late feature. The
pattern captures the already-parsed metadata once and directly mints the
concrete owner placeholder. It must not rescan the whole class declaration or
invoke `TokenTEMPLATE::parse` per specialization. Existing member-body tsubst
continues to consume the retained member declaration recipe.

### 4.7 Deferred and eager function bodies

During a class parse, `deferred_function_body_sink` collects method bodies and
constructor initializer spans. At class completion each body is either:

- entered in `deferred_lazy_bodies` by emit symbol; or
- parsed eagerly into `ast` and `pending_funcs`, depending on body provenance
  and function-template depth.

Class-KIND only replaces the class shell and member-declaration parse. It does
not parse a method body during class instantiation. For the first landing, each
method pattern keeps the current raw body recipe; the class substitution walk
applies the class binding to that recipe without parsing and attaches the same
`DeferredFunctionBody` shape the parser produces. Existing ODR-use materialize
and function-body tsubst behavior remains authoritative.

Any shape that would require an eager body parse during eligible class
instantiation is initially ineligible. Later body widening belongs on the
existing function tsubst spine, not in the class walker.

### 4.8 Friends and synthesized comparisons

Class parsing records friend class/function names, hoists hidden-friend operator
definitions, and synthesizes defaulted `operator==`/`operator<=>` after the
member list is complete. Those paths also mutate namespace function maps,
overload sets, `pending_funcs`, and free-operator metadata.

They are explicit declaration KINDs. The first slice marks body-bearing friends
and defaulted-comparison synthesis ineligible. Later widening must build the
free-function declaration/body recipe structurally through shared function
registration. Calling `parse_hoisted_friend_operator` from an admitted class
pattern would be a parser fallback and is forbidden.

### 4.9 Completion, runtime, forest, and post-instantiation hooks

After the body, the current paths also:

- run `compute_layout`, `apply_member_layout`, and `build_vtable_groups`;
- set `is_complete` even for an empty class;
- allocate the runtime vtable pointer array when needed;
- record the completed aggregate and method signatures through
  `forest_arena_record_aggregate` / `forest_arena_record_func`;
- register typedef/trailing variable forms for struct parsing;
- attach captured out-of-line member definitions;
- instantiate out-of-line nested-class definitions;
- repair qualified/legacy alias keys to point at one object;
- complete pending instantiations when a previously forward-declared template
  definition appears;
- verify that the concrete registration key exists.

The structural path calls the same completion and forest write-through helpers
after all concrete state is final. Out-of-line member body attachment is
supported as a body-recipe operation. A template with an out-of-line nested
class remains ineligible until that nested definition is also normalized into a
`ClassPattern`; the current parser-based nested-class replay is not allowed
under an admitted outer pattern.

## 5. Pattern representation

### 5.1 Ownership

`TemplateDef` is copied during lookup, merging, partial-specialization
selection, and instantiation. It must not own a move-only tree directly.

Add a Program-lifetime `ClassPatternArena` and store a stable
`ClassPatternId` in each body-bearing `TemplateDef`. Primary and partial
specializations have independent ids and eligibility. A zero id means no body
or a legacy forest that is rejected by the format-version gate; it is not a
signal to rebuild a pattern at bind time.

Pattern nodes use indices and immutable value fields. They do not retain
temporary parser map keys or raw pointers to temporary pattern aggregates.
Concrete external types may be referenced through a structural type reference
that serializes as the existing type id.

### 5.2 TypePattern

Use an enum, not strings, for this finite type grammar:

```text
CONCRETE_TYPE       existing non-dependent DataDef/type id
TEMPLATE_PARAM      template-parameter index
SELF_TYPE           this class-pattern node
NESTED_TYPE         class-pattern-local nested node id
POINTER             operand
REFERENCE           operand
CONST_TYPE          operand
C_ARRAY             element plus folded dimensions/expression recipe
FUNCTION_POINTER    return plus parameter patterns
TEMPLATE_ID         template identity plus argument patterns
DEPENDENT_MEMBER    owner type pattern plus member name/KIND
PACK_EXPANSION      operand plus pack-parameter index
```

`TEMPLATE_ID` and `DEPENDENT_MEMBER` are the class equivalents of the existing
`dependent_shell_origin` and `dependent_derived_origin` records. The normalizer
must use those provenance maps rather than treating an opaque placeholder name
as a concrete type.

A `TypePattern` that cannot be expressed by the finite grammar makes the whole
class pattern ineligible with a stable reason. A substituted result containing
`TEMPLATE_PARAM`, unresolved `DEPENDENT_MEMBER`, or an opaque pattern-only shell
is a loud internal error before registration commits.

### 5.3 DeclarationPattern

Source order is semantic for aliases, overload ranks, member layout, vtable
slots, and nested lookup. Store one ordered vector with enum KINDs:

```text
TYPE_ALIAS
NESTED_AGGREGATE
NESTED_FORWARD
NESTED_ENUM
DATA_MEMBER
BIT_FIELD
ANONYMOUS_AGGREGATE
STATIC_DATA_MEMBER
METHOD
MEMBER_TEMPLATE
FRIEND_TYPE
FRIEND_FUNCTION
DEFAULTED_COMPARISON
USING_BASE_MEMBER
STATIC_ASSERT
```

Access is normalized onto each declaration; access-label nodes are unnecessary.
Ignored specifiers/attributes are retained only when they affect semantic state
or emission. `STATIC_ASSERT` may be a no-op node after it has been evaluated
once at definition capture, but dependent assertions require a later expression
KIND and are initially ineligible.

## 6. Parse-once capture

Capture runs when `TokenTEMPLATE::parse` has collected a complete primary or
partial class body, before `register_template` publishes the `TemplateDef`.
Forward declarations have no pattern.

The capture reuses the production aggregate parser once under:

- `TemplateParamScope` for the class template parameters;
- `dependent_parse_in_progress`, preventing eager nested instantiation;
- a unique stable internal pattern identity derived from template identity,
  not a global counter;
- isolated compound, typedef-shadow, class-scope, namespace, diagnostic, and
  token-stream state;
- `forest_arena_enabled` disabled for temporary semantic objects;
- a registration journal and a muted pack-tap scope.

The registration journal is required. The production parser needs temporary
map visibility for self references and nested lookup, but no temporary pattern
name may escape into:

- `struct_map`, `datatype_map`, `namespace_datatype_map`, or scoped typedefs;
- Program Variables or `funcdef_map`;
- `template_map`, partial/alias/function-template maps, or overload sets;
- `top_decls`, `ast`, `pending_funcs`, or deferred-body maps;
- function-local identity maps;
- pack decl-index taps;
- project type ids or forest records.

Before implementing capture, registration writes used by the aggregate parser
must be routed through existing or newly extracted chokepoints that can journal
their undo. This is a behavior-neutral prerequisite, not a second parser.
Mutations of pre-existing concrete types must either be explicitly valid
definition-time work or be journaled too; an escaped mutation makes capture
ineligible and fails a debug assertion.

After the one parse, normalize the temporary `DataDefSTRUCT`/`DataDefCLASS`,
method declarations, nested types, aliases, and deferred-body records into the
immutable pattern. Then roll back the temporary registrations. Parser failure,
poisoned dependent resolution, unsupported declaration KIND, or unnormalizable
type provenance records one eligibility reason on the `TemplateDef`.

No instantiation is attempted during capture, and no pattern is rebuilt later.

## 7. Eligibility and the no-fallback rule

Eligibility is a property of the selected primary/partial pattern, computed at
capture or restored verbatim from the forest. It is not a speculative result of
trying an instantiation.

Initial stable reason enum:

```text
PATTERN_PARSE_ERROR
PATTERN_PARSE_POISONED
UNNORMALIZABLE_TYPE
DEPENDENT_VALUE_EXPRESSION
UNSUPPORTED_DECL_KIND
UNSUPPORTED_NESTED_DEFINITION
UNSUPPORTED_FRIEND_DEFINITION
UNSUPPORTED_DEFAULTED_COMPARISON
UNSUPPORTED_OUTOFLINE_NESTED
REQUIRES_EAGER_BODY_PARSE
REGISTRATION_ESCAPE
```

The enum is rendered to `[why: ...]` text only at the stats boundary.

The dispatch rule is:

```text
if cache hit:
    return cached specialization
if dependent/opaque binding:
    return or create the opaque shell
if selected pattern is ineligible:
    increment class-parse counter/reason
    run the legacy token parser as the only instantiation path
else:
    increment class-pattern counter
    instantiate ClassPattern transactionally
    on any substitution/completion failure: rollback and fail loudly
```

There is no `try pattern; catch; parse tokens` branch. That would recreate the
deleted function-body fallback under another name.

## 8. Structural instantiation algorithm

### Phase A: prepare and pre-register

1. Start a `ClassInstantiationTransaction` that journals every public map,
   variable, deferred-body, tap, and type-table write.
2. Locate a compatible incomplete object under the registered or legacy alias
   key; otherwise allocate the source-flavor shell.
3. Set canonical spelling, namespace, enclosing class, source provenance, and
   dependent-surface flags.
4. Register the root shell through the shared registration helper.
5. Allocate and pre-register nested shells in pattern source order.

This preserves self/nested references and existing forward-object pointer
identity.

### Phase B: substitute types and declarations

1. Build the placeholder binding directly from `ClassTemplateBinding`, using
   the same parameter-index representation as function tsubst.
2. Substitute direct bases, recursively instantiating dependent template-id
   bases through the same class-pattern entry point.
3. Initialize concrete base state immediately through the shared base helper:
   flatten members, build the virtual-base closure, and inherit method/vtable
   state. This must precede own declarations, matching `TokenCLASS::parse`, so
   override detection, overload lookup, and source-order vtable slots see the
   inherited surface.
4. Apply aliases in source order. Resolve dependent member types through the
   existing dependent-member-type machinery, not name surgery.
5. Fill nested patterns recursively.
6. Materialize data/static members through existing aggregate helpers.
7. Materialize method and member-template declarations through the shared
   signature/registration helper.
8. Attach substituted deferred body/default-argument recipes without parsing
   them.

Pack expansion is explicit on `TypePattern`/declaration nodes. Fan-out uses the
bound pack vector, preserving source order. It is not implemented by injecting
multiple token copies.

### Phase C: complete

1. Apply concrete struct-to-class promotion if required.
2. Run `compute_layout`, `apply_member_layout`, and `build_vtable_groups`.
3. Mark complete and allocate runtime vtable storage if needed.
4. Bind declaration-only C++ symbols and validate overload identities.
5. Attach out-of-line member bodies and supported structural nested patterns.
6. Repair alias keys so every spelling points to the same object.
7. Record the final state in the forest arena and commit the transaction.

Completion must assert that no pattern placeholder or temporary pattern object
is reachable from the concrete aggregate, its bases, aliases, members, methods,
defaults, or nested types.

## 9. Forest persistence

Forest binding is SAVE STATE / LOAD STATE. The pattern parsed by the producer is
state and must be restored directly.

Preferred representation:

- keep the existing `CIR_TMPLK_CLASS` and `CIR_TMPLK_PARTIAL` template record
  kinds;
- extend `cir_forest_template_record` with a pattern-payload slice and stored
  eligibility reason;
- encode immutable class/type/declaration pattern words in the existing
  `TEMPLATE_PAYLOAD` segment;
- reference raw method/default/body token runs through the existing
  `TEMPLATE_TOKENS` segment rather than duplicating bytes;
- serialize concrete external type references with existing type ids,
  template parameters by index, and nested patterns by local node index;
- apply the existing TU-root fence to the complete template record and its
  pattern;
- bump `CIR_FOREST_FORMAT_VERSION`; stale containers are rejected.

This adds no new segment and no new top-level template record KIND. It is still
a container layout change and requires owner review before implementation. If
the owner does not approve carrying `ClassPattern` in the existing template
payload, the bound half of Slice B is blocked. Re-parsing the restored body or
silently marking every restored template ineligible is not an acceptable
workaround.

The existing body token run remains while any pattern can be ineligible; it is
the sole-parse source for that pattern and useful diagnostic provenance. Its
eventual deletion is a separate zero-burndown decision.

## 10. Counters and ratchet

Add `--show-stats` output at the class-instantiation dispatch point:

```text
[stats]   class instantiate ... P pattern / Q parse / R cache / S opaque
[stats]   class parse profile (ranked):
[stats]     1. N x namespace::Template [why: reason] sample=Concrete<Args>
```

Definitions:

- `pattern`: a concrete specialization built from `ClassPattern`;
- `parse`: a concrete specialization whose selected pattern was known
  ineligible before work began, so the parser was its only path;
- `cache`: an already-complete specialization returned without work;
- `opaque`: a dependent shell, not a failed concrete instantiation.

Do not count recursive calls twice at one dispatch site, and do not classify a
cache hit as a pattern hit.

Add a suite script parallel to `tsubst_burndown.sh` that records total pattern
and parse instances plus distinct reason classes. The checked-in baseline is
updated only when a supported declaration/type KIND intentionally lowers the
parse count. Gates:

- total parse count may decrease or stay flat, never increase;
- a new reason class fails;
- an admitted pattern failure is always a test failure, never a reason row;
- live and packed runs report compatible engagement for the same consumer
  specializations, accounting for producer-restored cache hits.

## 11. Equivalence validation

### 11.1 Semantic fingerprint

Add a test helper that fingerprints a completed aggregate without pointer
addresses. Include:

- root names/canonical spelling/flavor/flags/size/alignment;
- every member parallel field, alias, static member/value, and nested type;
- bases, virtual-base offsets, inherited origins, and vtable groups;
- methods in order, method-map keys, ctor set, Variable flags, FuncDef return
  and parameter types, defaults, display/local/emit symbols, declaration flags,
  and deferred-body presence;
- friend and polymorphism state;
- registration keys and alias-key identity relationships.

During implementation, compare two separate Program runs of the same source:
normal pattern eligibility versus the still-existing ineligible sole-parse
lane. Do not run both implementations inside one production instantiation.
Temporary force-legacy test scaffolding must have an explicit deletion slice.

### 11.2 Language and ABI oracles

For each widening KIND:

- runtime output matches GCC and clang;
- `sizeof`, `alignof`, member offsets, base offsets, and virtual layout match
  GCC/clang for the covered shape;
- method/ctor/dtor/operator Itanium symbols match the existing parser and the
  compiler oracle;
- `--emit=c11` and `--dump-mir` are identical between the pattern and sole-parse
  runs where deterministic identity is expected;
- live, bound, and packed consumers match;
- whole-TU bind byte-identity gates remain green.

### 11.3 First target matrix

Start with small user templates that isolate:

1. scalar and pointer data members;
2. aliases and dependent member aliases;
3. one dependent base;
4. nested aggregate plus nested alias;
5. ctor/dtor/ordinary/static/operator declarations;
6. a member-template declaration with a deferred body;
7. forward completion and two specializations in one TU.

Then run a producer-time eligibility census over the exact
`vector<int32_t>`/`testsubscript` chain. The first performance landing must cover
the complete closed KIND set that chain actually uses, including
`_Vector_base`'s nested `_Vector_impl_data` and `_Vector_impl`, dependent aliases,
multiple bases, constructors, and member-template declarations. Do not claim a
vector win from a toy template while the measured seven specializations remain
on the parse lane.

## 12. Implementation slices

Each slice is its own commit with a green tree. No implementation belongs in
this design commit.

1. **B0: registration and observability foundation.** Extract shared shell,
   method-signature, base-flatten, and completion helpers; add journaling and
   counters. Behavior stays on the sole-parse lane. Establish the exact class
   parse census.
2. **B1: pattern capture and forest carry.** Add `ClassPatternArena`, dependent
   capture/normalization, existing-template-payload serialization, restore, and
   semantic fingerprint tests. Keep all patterns ineligible until round-trip
   equality is proven. Requires owner approval for the format extension.
3. **B2: basic structural instantiation.** Cover concrete/template-param unary
   types, aliases, scalar/pointer/array members, simple methods, and forward
   completion. Admit only those complete patterns; eligible failures are loud.
4. **B3: vector closure.** Add dependent template/member types, nested
   aggregates, dependent/multiple bases, constructors/destructors/operators,
   member-template declarations, and out-of-line body attachment as required by
   the measured vector census. Land only when the seven-specialization parse
   count falls and both live and bound timing improve.
5. **B4: string closure.** Widen default arguments, static members, unions,
   anonymous aggregates, and other KINDs demonstrated by the string census.
6. **B5: map/set/list closure.** Widen deeper nested/member-template and
   virtual/friend surfaces shown by those chains.
7. **B6: remaining reason burn-down.** Add friends/defaulted comparisons,
   dependent value expressions, explicit/partial-specialization edge cases,
   and out-of-line nested types by generic KIND until the suite class-parse
   tally reaches zero. Delete temporary force-legacy scaffolding and, in a
   separately reviewed change, the obsolete class token-reparse machinery.

No slice may add a name-keyed exception. A template that appears only in an
ineligibility row is unfinished coverage, not a supported special case.

## 13. Gates and performance protocol

For code slices, use the repository's one final heavy validation pass rather
than repeating a just-completed full test without a change:

```text
make -C src fulltest
make -C src release
MADC_BIN=bin/madc-release bash scripts/run_tests.sh
bash scripts/forest_bind_gate.sh
```

`fulltest` already includes the bind gate; do not immediately rerun that gate
unless diagnosing it or validating a later release-only change. Always verify
that `bin/madc-release` contains the forest blob; a blob-less release silently
runs the packed suite live.

For profiling, use `MADC_CPU_LIMIT` explicitly and compare the existing dev and
release binaries. Record `scripts/forest_phase_bench.sh` rows at landed
performance milestones when system load is below 2.

First performance acceptance:

- the measured seven class-body parser calls move materially to the pattern
  lane;
- both live and bound `testsubscript` instantiate time and total wall decrease;
- no regression in release size or forest compression policy;
- semantic fingerprints and GCC/clang layout/symbol oracles match;
- fulltest, packed suite, bind byte-identity, and class/tsubst ratchets are green.

## 14. Explicit non-goals

- No implementation in this design sitting.
- No explicit-instantiation prelude TU (Option C remains owner-gated).
- No new forest segment or top-level template record KIND in the preferred
  design.
- No parser call hidden behind an eligible declaration KIND.
- No second layout, mangling, overload, or aggregate-registration
  implementation.
- No class/template-name special cases.
- No change to the approved zstd/transform/intern-spine compression design.

## 15. Owner checkpoint

Before B1, obtain explicit review of the proposed format-version bump and the
extension of the existing class-template payload to carry `ClassPattern`.
Everything else in the plan is internal front-end refactoring or ratcheted KIND
widening. If that payload extension is rejected, stop and redesign the durable
pattern representation; do not substitute a bind-time parse.
