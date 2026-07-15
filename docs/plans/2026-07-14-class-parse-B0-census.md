# Class parse-once B0 census

Recorded on 2026-07-14 from `feature/class-parse-once-codex` after the B0
instrumentation landed in the working tree. This is the sibling census required
by the class-KIND parse-once design and handoff.

## Counter semantics

The dispatch counters classify one outcome at the concrete class-template
instantiation boundary:

- `pattern`: a specialization built from a captured `ClassPattern`;
- `parse`: a specialization known before work begins to require the sole parser
  lane;
- `cache`: an already-complete specialization returned without work;
- `opaque`: a dependent shell, not a failed concrete specialization.

The body, base-specifier, and declaration-KIND counters measure parser work below
an active `parse` dispatch. Nested work remains attributed to the specialization
that selected the parser lane. Consequently, body counts need not equal dispatch
counts. A cache hit is never counted as a pattern hit.

## Suite-wide baseline

`MADC_CPU_LIMIT=30 bash scripts/class_parse_burndown.sh --check` produced:

| Metric | B0 baseline |
|---|---:|
| Tests with stats | 690 |
| Project-mode tests skipped | 5 |
| Tests exercising class instantiation | 286 |
| Pattern | 0 |
| Parse | 48,604 |
| Cache | 99,334 |
| Opaque | 24,430 |
| Approved parse reasons | `pattern-not-captured` only |

The machine-readable ratchet is
`docs/parity/class-parse-baseline.txt`. Later slices may lower `parse`, but may
not raise it or introduce an unapproved reason.

## `testsubscript` container chain

The exact samples were collected with:

```text
MADC_CPU_LIMIT=120 bin/madc --show-stats tests/testsubscript.mad
MADC_CPU_LIMIT=120 bin/madc-release --show-stats tests/testsubscript.mad
```

The release binary contained a valid appended forest (`MADCSNAP` in its final
32 bytes). The bound sample therefore measures restored producer state plus
consumer-time specialization, rather than a silent live-parse fallback.

| Metric | Live development binary | Bound release binary |
|---|---:|---:|
| tsubst bodies | 35 hit / 0 fallback | 34 hit / 2 fallback |
| Pattern | 0 | 0 |
| Parse | 450 | 253 |
| Cache | 722 | 338 |
| Opaque | 153 | 55 |
| Class bodies parsed | 494 | 276 |
| Base-specifier lists parsed | 196 | 103 |
| Parse reason | `pattern-not-captured` | `pattern-not-captured` |

Forest restore removes producer header parsing and changes the available cache,
which accounts for the different live and bound totals. Both modes still send
every newly required concrete class specialization through the parser because B0
captures no class pattern.

## Declaration-KIND census

These are dynamic declaration visits below parser-lane specializations in the
same `testsubscript` samples. Zero rows are retained so later slices cannot
mistake an unobserved KIND for implemented coverage.

| Class declaration KIND | Live | Bound |
|---|---:|---:|
| `TYPE_ALIAS` | 1,542 | 591 |
| `NESTED_AGGREGATE` | 45 | 29 |
| `NESTED_FORWARD` | 4 | 0 |
| `NESTED_ENUM` | 15 | 2 |
| `DATA_MEMBER` | 245 | 88 |
| `BIT_FIELD` | 0 | 0 |
| `ANONYMOUS_AGGREGATE` | 8 | 0 |
| `STATIC_DATA_MEMBER` | 38 | 2 |
| `METHOD` | 3,858 | 1,100 |
| `MEMBER_TEMPLATE` | 1,254 | 635 |
| `FRIEND_TYPE` | 18 | 2 |
| `FRIEND_FUNCTION` | 54 | 24 |
| `DEFAULTED_COMPARISON` | 0 | 0 |
| `USING_BASE_MEMBER` | 81 | 35 |
| `STATIC_ASSERT` | 43 | 29 |

## Coverage consequences

B1 must round-trip an immutable representation of every observed KIND, even
though all patterns remain ineligible in that slice. B2 can admit only its
approved basic subset: aliases, scalar/pointer/array members, simple methods,
and forward completion.

B3 cannot claim the `testsubscript` vector payoff until the closed vector chain
also covers nested aggregates and enums, dependent and multiple base lists,
constructors/destructors/operators represented by `METHOD`, member-template
declarations, inherited-member `using` declarations, and dependent static
assertions as demanded by the selected patterns. Static data, anonymous
aggregates, and friend surfaces remain measured work for the later string and
map/set/list closures unless the vector eligibility closure proves they are
required earlier. No template name is an eligibility exception; widening stays
generic by KIND and type-expression shape.
