# Per-signature vtable slots

Owner go: "Yea do it properly" (2026-10-21).

## Defect

`DataDefCLASS::vtable_slots` holds one entry per virtual NAME. Overloaded
virtuals share one slot; an override of one overload takes over every
overload of that name (`method_map[name]` holds the last-declared one).
Reducer `tests/testvirtualoverloadslots.mad`: g++ 13 and clang++ 18 print
`1 20 3 4 50 / 10 2 / 60 70 / 1 20`; madc prints
`20 20 3 50 50 / 10 10 / 60 60 / 20 20`.

A library class's vtable is the library's, so madc's slot INDEX must be
Itanium's. libc++ `ctype<wchar_t>::widen` (inline) calls `do_widen(char)`
through slot 8 where Itanium has it at 11, so every `wcout <<` crashes
(teststreamwideops, darwin and Linux `-stdlib=libc++`).

## Canon

Itanium C++ ABI 2.5.2: one entry per virtual function in declaration
order; a destructor takes two (complete, deleting) at its position; an
override ([class.virtual]/2: same name, parameter-type-list, cv- and
ref-qualifier) reuses its base's entry.

## Design

1. `FuncDef::virtual_slot_key()`: the display name + each user parameter's
   Itanium encoding (`mangle_param_spelling` -> `itanium_encode_type_sub`;
   the mangler already desugars typedefs and template parameters, e.g.
   `_ZN1B1fE3Foo` for a `Bar` typedef parameter) + `K`/`V`/`R`/`O`.
   A destructor keeps its display name (its slots stay `~` /
   `~$deleting`). A misaligned spelling array falls back to the display
   name (the old key).
2. Registration (`register_class_method_signature`): a non-static method
   with a virtual name takes a slot only if it is declared `virtual` or
   its key names an inherited slot. `virtual_methods` stays the NAME index
   (cheap prefilter); `vtable_slots` / `vtable_groups` hold keys.
3. `DataDefCLASS::slot_overrider(key)` (beside `findMethod`): the final
   overrider -- this class's own method with that key, else the dominant
   candidate over every base. Replaces `findMethod(slot)` in
   `class_vtable_def`, `note_vtable_slot_references`,
   `class_pure_virtual_of`.
4. CIR dispatch: `method_slot_name` returns the callee's key (a static
   member: none); the pointer-to-virtual-member constant reads the member
   FuncDef's key; a thunk is named by its target body symbol.
5. `is_externally_defined`: a bodied method occupies a slot iff its key does.
6. Forest format v74: `vtable_slots` strings change meaning.

## Thread safety

Parse-time class state, written once per class by the parsing thread; no
new shared state.

## Tests

- `tests/testvirtualoverloadslots` (header-free classes): overloads, an
  override of one overload, cv overloads, name hiding without
  `using`, a virtual-base diamond (dominance), pointers to overloaded
  virtual members, typedef / template-parameter spellings.
- `teststreamwideops` under `-stdlib=libc++` (Linux) and on the darwin lane.
