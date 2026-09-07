# Dialect Literals — reasoning

## What this rule protects

madc's dialect has first-class object and array literals — `var x = { "k": v }`,
`var a = { e0, e1 }` — and a deliberate, substantial investment went into making
them work well. Writing an object by declaring a bare `var x;` and then filling
it a field at a time:

```
var rq;
rq["kind"] = "run";
rq["doc"] = doc;
rq["pause"] = 1;
```

is the same object spelled the long, junior-dev way. It is harder to read (the
shape is not visible in one place), and — the concrete cost that motivated this
rule — it forces the work to be done twice: once in the degraded form, then a
refactor pass to bring it up to the literal form. The literal expresses it once:

```
var rq = { "kind": "run", "doc": doc, "pause": 1 };
```

## The 2026-09-07 regression that motivated the gate

While editing `spans_to_hspans` (madcide), an agent rewrote a working object
literal (`hs[] = { "s": s, "e": …, "c": c };`) into imperative
`var row; row["s"] = …; row["cls"] = …;` — reversing a whole prior session's
refactor of exactly this pattern, on the very file being edited. A rule with no
gate decays; this gate makes the degraded form fail the build so neither a human
nor an agent can reintroduce it silently.

## Why the exclusions exist (the gate's precision)

Imperative key-assignment is a legitimate, tested language feature — it is how
you MUTATE an object. The gate therefore flags only the unambiguous regression
(a bare, uninitialized `var NAME;` immediately followed by a string-literal-key
assignment) and deliberately spares:

- **Mutation of an initialized var** (`var x = { ... }; x["k"] = v;`) — adding a
  field conditionally, in a loop, or after logic. The literal came first; the
  rest is honest mutation.
- **Computed / dynamic keys** (`fresh[key.c_str()] = action;`) — a literal
  cannot express a key computed at run time.
- **Integer / array indices** (`cases[0] = stopc;`) — arrays are a separate
  concern; array building via `.push` / index is a fine idiom.

Two feeder tests of the value carrier (`tests/testvaluekeys.mad` and kin)
exercise `var o; o["k"] = v;` on purpose — that is the feature under test — so
the gate is scoped to production `tools/` code, not `tests/`.

## The ternary-in-literal trap

A literal value that is itself a ternary (`{ "lo": a < b ? a : b }`) puts a `:`
inside a value where the parser also uses `:` to separate key from value. There
was no precedent for it in the tree. Compute a local first and put the local in
the literal — clearer, and no parse ambiguity.

## Scope and mechanism

`scripts/check-dialect-literals.sh` scans `tools/**/*.{inc,mad}` and self-tests
in both directions: a synthetic bad file must fail the scan, and a synthetic
good file (initialized-var mutation + computed key + integer index) must pass.
A rule without a gate decays; a gate without a negative control lies.
