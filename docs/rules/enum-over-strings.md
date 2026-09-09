# Enums Over Strings — Reasoning

## The failure case

The transpiler's type classification system was introduced as a quick
hack using single chars (`'c'`, `'s'`, `'d'`, `'i'`) to tag variable
types for `cout` format selection. This spread through the sema pass
and emitter — ~50 call sites doing char comparisons instead of enum
dispatch.

Problems:
- **No compile-time checking.** A typo like `'C'` instead of `'c'`
  silently produces wrong output.
- **Not extensible.** Adding a new type class (e.g. `TC_CLASS`,
  `TC_POINTER`) means inventing a new char and hoping it doesn't
  collide.
- **Slow in hot paths.** The `classify_type()` function does 6+
  `std::string::find()` calls per invocation. In the emitter's cout
  chain, this runs per-expression per-value. An enum comparison is
  one integer compare.
- **External ASTs and tool outputs name nodes with C strings** (the
  removed Gecko parser experiment was the motivating case). Converting
  them to enums at the boundary (one `strcmp` per node) and dispatching
  on the enum internally is strictly faster than doing `strcmp` at
  every use site.

## The rule

Use enums for any classification that appears in more than one place.
Convert string-based discriminators to enums at system boundaries
(lexer output, external-tool ASTs, user input) and never pass the
string deeper.

## When strings are OK

- One-off debug/error messages
- User-facing output
- Map keys for genuinely dynamic lookups (symbol tables, etc.)

## The 2026-09-09 instance: keys, actions and targets in the IDE

The owner restated the rule after reviewing the madcide / ui layers:
"I've noticed you've been using strings all over the place for things that
should be enums or numeric constants… things like keyboard keys… which is
problematic in two ways: if it's a string and not an established
constant/enum then a misspelling or non-existent/undefined code will go
undetected; also it is much more expensive to do a string comparison than a
numeric comparison or numeric switch/case."

What was string-shaped at the time:

- The engine has a real key enum (`tui_key`, mirrored as `ui::key` in
  `bits/ui_enums`), but the event a dialect handler receives carried the key
  as its NAME, and `apply_ide_event` compared `ev["key"] == "enter"` and
  friends on every keystroke.
- A `.keys` profile maps a key spelling to an ACTION NAME, and the
  dispatcher was a ladder of string compares on that name. A misspelled
  action in a profile was discovered only when someone pressed the key.
- The view (`"mc11"`), the layout region (`"editor"`, `"panel"`), the panel
  tab, the diagnostics pane and the ui target (`"term"`, `"web"`) were
  string discriminators in IDE state. The emitter side already did it
  right: `cir_emit_lang_of` converts `"c11"` to `CirEmitLang` at its
  boundary and the renderer switches on the enum.

The boundary form of the rule follows from the two harms. Text arrives from
outside the program — a profile file, the page's JSON, a command line, a
manifest — and that is the ONLY place it may exist as text. Convert it once,
when it is read, into an enum or an interned id; a spelling the program does
not know is a refusal WITH the offending line, at load time, instead of a
key that silently does nothing at use time. From there on the code is an
integer: a `switch` in the engine, a `switch` in dialect code, an O(1)
compare in a hot path. The command registry stays DATA (the owner's
key-bindings-are-never-hard-coded law is untouched); what changes is that a
name is resolved to an identity once, not compared at every use.

The UI level is the same rule applied to the target: `ui::open("term")` /
`ui::open("web")` were name strings; the level is an ordered enum whose
order has a meaning (a composition targets the highest level the
application wants; every lower level ignores the hints it cannot show; a
target below the requested level refuses with the reason or serves the lower
rendering by the program's choice). Input devices and chrome are feature
flags beside the level, so two GUI kinds that differ only in devices do not
fork the ladder (the hub design's "capability levels plus feature flags").

The conversion of the existing sites is owned by the client-server arc
(KG Decision `enums_not_strings_everywhere_owner_law`, Feature
`nexus_client_server_arc`): keys and actions before V1, the state
discriminators with V1's View representation enum. The gate that keeps it
converted is part of that slice — a rule without a gate decays.
