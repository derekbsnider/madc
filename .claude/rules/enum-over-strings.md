# Enums Over Strings for Classification

- Never use single chars or strings as type/category discriminators
  when an enum will do. Enums are compile-time checked, zero-cost
  comparisons, and self-documenting.
- String lookups in hot paths (type classification, format selection,
  AST node dispatch) are O(n) per comparison. Integer/enum dispatch
  is O(1).
- When interfacing with C-string node/type names from an external
  tool or AST, convert to an enum at the boundary — once — then use
  the enum internally.
- New classification systems must start as enums. Do not introduce
  `char`-based or `string`-based type tags.
- OWNER LAW (2026-09-09, repeated): this applies to EVERYTHING of this
  nature — keyboard keys, action names, UI targets/levels, view /
  region / tab discriminators — in the engine AND in dialect code
  (`tools/`). A string code that is misspelled or undefined goes
  undetected; an enum is a compile-time or load-time error.
- Text is legal only at an INPUT boundary (a profile file, the page's
  JSON, a command line, a manifest). Convert it ONCE, at load, to an
  enum or an interned id; a misspelling is a REFUSAL at load, never a
  dead key at use. Dispatch is a `switch` on the code, not a ladder of
  string compares.
- A dialect event carries the engine's enum code for its key / kind /
  action; a handler switches on the code. Profile action NAMES resolve
  against the command registry when the profile loads.
- The UI level is the ordered enum `ui::NONE < ui::LINE < ui::TUI < ui::WEB <
  ui::GUI < ui::GFX2D < ui::GFX3D` in `include/madc/bits/ui_enums` (the one
  enum text for engine + dialect); a target declares the level it
  serves; devices and chrome are feature flags beside it.

See `docs/rules/enum-over-strings.md` for the reasoning.
