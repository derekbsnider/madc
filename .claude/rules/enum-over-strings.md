# Enums Over Strings for Classification

- Never use single chars or strings as type/category discriminators
  when an enum will do. Enums are compile-time checked, zero-cost
  comparisons, and self-documenting.
- String lookups in hot paths (type classification, format selection,
  AST node dispatch) are O(n) per comparison. Integer/enum dispatch
  is O(1).
- When interfacing with Gecko AST node names (which are C strings),
  convert to an enum at the boundary — once — then use the enum
  internally.
- New classification systems must start as enums. Do not introduce
  `char`-based or `string`-based type tags.

See `docs/rules/enum-over-strings.md` for the reasoning.
