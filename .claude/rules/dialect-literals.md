# Dialect Literals — construct objects with literals, not field-by-field assignment

- In madc-dialect PRODUCTION code (the shipped tools under `tools/`), build an
  object with an object literal: `var x = { "k": v, ... };`.
- NEVER declare a bare `var x;` and then populate it field-by-field with
  string-literal keys (`x["k"] = v; x["m"] = w;`). That is an object literal
  spelled imperatively — the degraded form.
- Imperative key-assignment is for MUTATING an object that ALREADY exists:
  a field added conditionally, in a loop, or after intervening logic; a
  computed/dynamic key (`x[name.c_str()] = v`); an array index (`x[0] = v`).
  Initialize with the literal first, then mutate.
- A field whose value needs logic first: compute a local, then place the local
  in the literal (`long lo = a < b ? a : b; var r = { "lo": lo };`). Do NOT
  embed a ternary directly in a literal value — its `:` collides with the
  key/value `:`.
- Object keys serialize sorted (the carrier is a `std::map`), so literal vs
  field-by-field construction never changes output — choose the literal.
- Do not degrade existing literal syntax back to imperative construction when
  editing a file; write the good form the first time.
- Gate: `scripts/check-dialect-literals.sh` (in fulltest) fails on a bare
  `var NAME;` immediately followed by `NAME["…"] = …`, over `tools/**/*.{inc,mad}`.
  It carries a two-way negative control.

See `docs/rules/dialect-literals.md` for the reasoning.
