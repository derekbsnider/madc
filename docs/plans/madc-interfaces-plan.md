# madc-interfaces: Language-Conventional Interfaces Plan

## Status

Active design track. This document defines the user-facing
vocabularies that operate on the `madcdis` data substrate. It is
paired with `madcdis-plan.md` (substrate) and `madcdat-plan.md`
(external drivers).

The architectural principle: every interface surface lowers to the
same `madcdis` runtime types and the same Query IR. Surfaces are
vocabularies, not islands. A user picks the vocabulary that matches
their mental model; the substrate sees uniform operations underneath.

## Intent

Mad-C's audience spans multiple programming traditions: C systems
programmers, modern C++ developers, dynamic-language users (Ruby,
Python, PHP), Objective-C developers, query-language users (SQL,
Cypher). Each group has established expectations for how to express
common data operations.

Rather than force one vocabulary on everyone, Mad-C offers multiple
syntactic surfaces over the same data substrate. The result:

- A Ruby developer writes `users.where { |u| u.active }.each { |u| ... }`
- A C# developer writes `users.where(...).select(...).order_by(...)`
- A C++23 developer writes `users | std::views::filter(...) | std::views::transform(...)`
- A Python developer writes `[u.name for u in users if u.active]`
- A SQL developer writes `sql::SELECT name FROM users WHERE active`
- A Cypher developer writes `cypher::MATCH (u:User {active: true}) RETURN u.name`
- An Objective-C developer writes `[users where:^(User &u) { return u.active; }]`
- A C developer writes the canonical `DataSet<T>` API directly

All compile to the same operations against the same datasets. Results
from one surface compose with operations from any other surface.

## Non-Goals

- No surface has its own data engine. All surfaces lower to the
  shared substrate.
- No surface is privileged in capability. Users can express the
  full data model through any surface (subject to that surface's
  natural conventions).
- No requirement to use a specific surface. The default is the
  C-native core; everything else is opt-in via namespace, header
  include, or grammar pragma.
- No automatic translation between surfaces in user code. A user
  writing Ruby-style code doesn't get the same code transformed to
  LINQ-style; the lowering happens to Query IR, not between
  surfaces.

## Core Principles

### 1. C++ is the canonical foundation

Mad-C is a C extension with C++ idioms. The C-native and C++-native
APIs are not "one of many surfaces" — they're the canonical form
that every other surface lowers to. When two surfaces interact, they
do so through C-native types (DataSet, Cursor, Relation, Query).

### 2. All surfaces share one Query IR

When a surface needs to express a query (filter, projection,
ordering, join), it produces a `madc::Query` IR object. The planner
and executor only see this IR. This means:

- Bug fixes in the planner benefit every surface
- Performance improvements apply uniformly
- Federation works for queries from any surface
- Surfaces don't fragment the language's capabilities

### 3. Surfaces are namespace-prefixed

Following Mad-C's broader namespace discipline (`php::`, `ruby::`,
`rust::`), each surface lives in its own namespace:

- `madc::linq::` — fluent method chaining (C# / Rust / JS / modern C++)
- `madc::objc::` — Objective-C bracket dispatch
- `madc::ruby::` — Ruby trailing-block syntax
- `madc::python::` — Python comprehensions and idioms
- `madc::orm::` — ActiveRecord/Django-style typed records
- `sql::`, `cypher::`, `gql::` — textual query sub-grammars

Bare access via `using namespace madc::linq;` (or the equivalent for
other surfaces) is supported, with the same scoping rules as standard
C++ `using` directives.

### 4. C++23 ranges are the LINQ-style surface

Rather than build a parallel LINQ-style library, `madcdis` integrates
with C++23's `std::ranges` and views library. `DataSet<T>` and
`Cursor<T>` model the `std::ranges::view` concept; the standard
range adaptors compose with Mad-C data naturally.

This means modern C++ users get the LINQ surface essentially for free
— their existing knowledge of `std::views::filter`,
`std::views::transform`, etc. transfers directly to Mad-C data.

The `madc::linq::` namespace provides additional helpers that
integrate range pipelines with Mad-C-specific concepts (relations,
multi-dataset joins, query planner integration). Pure range pipelines
work without the namespace; the namespace adds bridges to features
ranges don't natively cover.

### 5. Compiler-integrated vs library-only surfaces

Some surfaces require Mad-C compiler frontend work; others are pure
library/template implementations:

**Library-only (work in any C++ compiler):**

- C-native core API
- C++23 ranges integration (`madc::linq::`)
- Objective-C-style brackets via macros and templates (`madc::objc::`)
- ORM-style records with declared relations (`madc::orm::`)
- Some Ruby-style method chaining on collections (`madc::ruby::`,
  partial)

**Compiler-integrated (require Mad-C frontend):**

- Native query syntax (`sql::`, `cypher::`, `gql::`)
- Ruby-shaped grammar mode (full do/end blocks, trailing blocks
  recognized natively)
- Python-shaped grammar mode (indentation, comprehensions,
  `def`/`class`)
- Objective-C-style brackets as native syntax (rather than macros)

The split matters for delivery phasing: library-only surfaces can
ship in any V1+ release as headers; compiler-integrated surfaces
align with the V4 compiler grammar work.

### 6. Results compose across surfaces

A `Cursor<T>` returned by any surface is usable by any other surface.
Concretely:

```cpp
// LINQ-style returns a Cursor<User>
auto active = users | std::views::filter([](auto &u) { return u.active; });

// Ruby-style consumes the same Cursor<User>
active.each { |u| println(u.name); }

// SQL-style can filter further
auto adults = sql::SELECT * FROM active WHERE age >= 18;
```

This is what makes the multi-surface story credible. Surfaces are
ways to *express* operations on shared data, not boundaries between
isolated data spaces.

### 7. Performance transparency

The user can always see what the runtime is doing. Every surface
supports `.explain()` (or equivalent) that shows the resulting Query
IR and execution plan. Surfaces don't hide the cost of operations;
they just provide convenient ways to express them.

This rules out the ORM trap (hidden N+1 queries, surprise full table
scans). The ORM surface specifically requires explicit eager-loading
declarations rather than lazy-load magic.

## The Surfaces

### Canonical: C-native core

The substrate API. Always available, no namespace prefix needed,
the foundation every other surface lowers to.

```c
DataSet<User> users("mem://users");

User u = users.get(user_id);
users.insert(new_user);

Cursor<User> cursor = users.scan();
while (cursor.next(&u)) {
    process(u);
}

Query q = query()
    .from("users")
    .where_eq("status", value("active"))
    .where_gte("age", value(18))
    .order_by("name")
    .limit(10)
    .build();

Cursor<User> results = users.query(q);
```

This is the form documentation references. Performance-sensitive
code stays here. Every other surface compiles to operations on these
types.

### C++23 ranges (LINQ-style fluent chaining)

The modern C++ surface. Uses standard ranges and views; no Mad-C-
specific syntax. Works in any C++23-or-later code.

```cpp
auto adults = users
    | std::views::filter([](const User &u) { return u.age >= 18; })
    | std::views::transform([](const User &u) { return u.name; })
    | std::views::take(10);

for (const auto &name : adults) {
    println(name);
}
```

`DataSet<T>` and `Cursor<T>` satisfy `std::ranges::view` so they
compose with standard adaptors. Lazy evaluation: the pipeline doesn't
execute until iterated.

The `madc::linq::` namespace adds Mad-C-specific helpers:

```cpp
using namespace madc::linq;

auto user_posts = users
    | filter([](const User &u) { return u.active; })
    | join(posts, on(&User::id, &Post::author_id))
    | order_by(&Post::created_at, desc)
    | take(20);
```

The `join` adapter is Mad-C-specific (it uses `Relation<A,B>` and the
planner's join logic). Standard ranges don't model relational joins
directly; this is where the Mad-C namespace adds value over plain
`std::views`.

### Objective-C brackets

Dynamic dispatch surface. Initially shipped as macros and template
functions (library-only); eventually promoted to native syntax via
compiler frontend.

```cpp
// V1-V3: macro-based
auto active = MADC_SEND(users, where, ^(User &u) { return u.active; });
auto names = MADC_SEND(active, map, ^(User &u) { return u.name; });

// V4+: native bracket syntax
auto active = [users where:^bool(User &u) { return u.active; }];
auto names = [active map:^(User &u) { return u.name; }];
```

The bracket form lowers to method dispatch through a runtime selector
table. This enables ActiveRecord-style magic-method dispatch on
`madc::value` objects (the `method_missing` pattern), forwarding
proxies, mock objects in tests, and other dynamic-dispatch patterns.

The selector lookup is cached; after the first call, dispatch is a
direct function pointer call. No measurable overhead vs. C++ virtual
calls.

### Ruby-style trailing blocks

Block-based iteration and querying. Library-only via templates and
operator overloading initially; full Ruby-shaped grammar in V4+
gives proper do/end syntax.

```cpp
// V1-V3: lambda-based, partial Ruby feel
users.where([](User &u) { return u.active; })
     .each([](User &u) { println(u.name); });

// V4+: native Ruby-shaped grammar (.rmad files or `using namespace madc::ruby;`)
users.where { |u| u.active }
     .each { |u| println(u.name) }

users.select { |u| u.age >= 18 }
     .map { |u| u.name }
     .first(10)
     .each { |name| println(name) }
```

The Ruby surface emphasizes:

- Trailing-block syntax (last argument is a block)
- `each`, `map`, `select`, `reject`, `reduce`, `find`, `any?`,
  `all?`, `count`, `group_by`, `sort_by`, etc.
- Methods ending in `?` for predicates, `!` for in-place mutations
- Optional parens on method calls

All compile to the same Query IR plus closure invocation as
LINQ-style. The vocabulary differs; the underlying execution is
identical.

### Python-style comprehensions

Comprehension and generator syntax. Requires Python-shaped grammar
mode (V4+).

```python
# .pymad file or `using namespace madc::python;` scope
adults = [u.name for u in users if u.age >= 18]
active_count = sum(1 for u in users if u.active)

users_by_status = {
    status: [u for u in users if u.status == status]
    for status in {'active', 'pending', 'suspended'}
}
```

Comprehensions lower to filtered iteration over the dataset. Generator
expressions lower to lazy cursors; list comprehensions materialize to
`madc::array<T>`. Dict and set comprehensions lower to `madc::map<K,V>`
and `madc::set<T>`.

The Python surface also provides:

- `len(dataset)` for size
- `dataset[key]` for indexed access
- `key in dataset` for membership
- Iteration with `for ... in ...`
- `enumerate`, `zip`, `range`, `sorted`, `reversed` as wrappers
  over the relevant Mad-C operations

### ActiveRecord/Django-style ORM

Typed record classes with declared relations. Library-only via
template inheritance and CRTP patterns.

```cpp
class User : public madc::orm::Record<User> {
public:
    Field<string> name;
    Field<int> age;
    Field<bool> active;
    HasMany<Post> posts;
    BelongsTo<Account> account;

    static DataSet<User> &dataset() {
        return madc::orm::registry<User>("shm://users");
    }
};

// Usage
User u = User::find_by(name, "alice");
for (auto &post : u.posts.recent(10)) {
    println(post.title);
}

User::where(active, true).order_by(name).each([](User &u) {
    println(u.name);
});
```

`HasMany<Post>` and `BelongsTo<Account>` are typed relation
declarations compiled to `Relation<A,B>` metadata at template
instantiation time. No runtime method-name parsing, no
`method_missing`. The "magic" is at compile time.

Critical discipline: relations are explicit, not lazy. `u.posts`
returns a query that the user must explicitly iterate, materialize,
or pass to other operations. There's no surprise database hit on
attribute access. This avoids the N+1 trap that dynamic-language
ORMs suffer from.

Eager loading is explicit:

```cpp
User::where(active, true)
    .include(&User::posts, &User::account)
    .each([](User &u) {
        // u.posts and u.account are pre-loaded
    });
```

The `include` directive tells the planner to fetch related data in
one query rather than per-row.

### Method chaining on collections

Universal "modern collection methods" available on standard
containers — works without any namespace, ships as part of `madcdis`.

```cpp
auto names = vec
    .filter([](const User &u) { return u.active; })
    .map([](const User &u) { return u.name; })
    .sort()
    .unique()
    .take(10)
    .to_vec();
```

This is the "every container has the same methods" discipline from
Ruby/Python/JavaScript. Available on `madc::array`, `madc::map`,
`madc::set`, `DataSet<T>` (when row-oriented), and any container
implementing the `Iterable<T>` concept.

The methods compose with `std::ranges` adaptors — a chain can mix
both styles. `vec.filter(...)` returns something that's also a
range-v3 / std::ranges-compatible view.

### Textual query languages

Already documented in `madcdis-plan.md`. Brief summary here for
completeness:

**`sql::`** — SQL sub-grammar, parses SQL statements to Query IR.

```cpp
auto adults = sql::SELECT name, age FROM users
              WHERE age >= 18 AND active = true
              ORDER BY age DESC
              LIMIT 10;
```

**`cypher::`** — Cypher sub-grammar, parses Cypher to Query IR with
graph patterns.

```cpp
auto network = cypher::MATCH (u:User {name: 'alice'})
                      -[:FRIEND*1..3]->(f)
                      RETURN f.name, f.age;
```

**`gql::`** — GQL sub-grammar (ISO/IEC 39075), the canonical query
language Mad-C's IR is shaped around.

```cpp
auto path = gql::MATCH (a)-[*]->(b)
            WHERE a.id = 1
            RETURN b
            ORDER BY b.weight DESC;
```

All three lower to the same Query IR and execute through the same
planner. They exist for compatibility with users' existing knowledge
and existing code; semantically, they're equivalent within the
expressive overlap of their grammars.

For dynamic queries (admin REPLs, user-input query builders), runtime
parser entry points exist:

```cpp
Query q = madc::parse_sql(query_string);
auto results = users.query(q);
```

These produce the same Query IR. They're a convenience for genuinely
dynamic queries, not the primary surface.

## Surface composition examples

Demonstrating that surfaces compose:

### LINQ + SQL

```cpp
// LINQ-style filter, SQL-style further refinement
auto active = users | std::views::filter([](auto &u) { return u.active; });

auto recent = sql::SELECT * FROM active
              WHERE last_login > now() - INTERVAL 7 DAY;
```

### Ruby + Cypher

```cpp
// Cypher graph traversal, Ruby-style post-processing
auto friends = cypher::MATCH (u:User {name: 'alice'})-[:FRIEND]->(f) RETURN f;

friends.select { |f| f.active }
       .group_by { |f| f.city }
       .each { |city, users_in_city|
           println("#{city}: #{users_in_city.count} friends");
       }
```

### Python + ORM

```python
# Python comprehension over ORM-defined relations
admin_users = [u for u in User.dataset() if u.account.role == 'admin']

posts_per_user = {
    u.name: u.posts.count() 
    for u in admin_users
}
```

### Objective-C + GQL

```cpp
// Objective-C dispatch on GQL results
auto graph = gql::MATCH (n)-[r]->(m) WHERE n.tag = 'hot' RETURN n, m;

[graph each:^(GraphRow &row) {
    [row.source notify:row.target];
}];
```

These examples are real — they would work in Mad-C with the surfaces
implemented as described. The vocabularies blend because they all
sit on the same substrate.

## Delivery Phases

The interfaces plan aligns with the substrate plan's phases. Each
phase delivers value on its own; later phases add capability without
rework.

### V1 — C-native core, C++23 ranges, method chaining

Land:

- Canonical C-native API (`DataSet<T>`, `Cursor<T>`,
  `Relation<A,B>`, `Query`, `QueryBuilder`)
- `std::ranges::view` integration for `DataSet<T>` and `Cursor<T>`
- `madc::linq::` namespace with Mad-C-specific helpers (join,
  relation-aware iteration)
- Method chaining on `madc::array`, `madc::map`, `madc::set`
  (.filter, .map, .reduce, etc.)
- `.explain()` support across the V1 surfaces

Outcome: every Mad-C program can express queries in the canonical
form or the LINQ-style form immediately. Most data manipulation
needs are covered. Users coming from modern C++, C#, Rust, JavaScript
feel at home.

### V2 — Objective-C brackets (library-only), Ruby-style methods

Land:

- `madc::objc::` macros and template machinery for bracket-style
  dispatch
- Runtime selector table for `madc::value` objects
- `method_missing`-style dispatch hook
- `madc::ruby::` with lambda-based block conventions (.each,
  .select, .reject, .find, .any?, .all?, predicate/mutator naming
  conventions)
- Result composition across V1 and V2 surfaces

Outcome: dynamic-dispatch and block-based vocabularies are
available. Ruby developers and Objective-C developers can write
recognizable Mad-C code, though without the full grammar mode
syntax yet.

### V3 — ORM surface

Land:

- `madc::orm::Record<T>` base class with CRTP
- `Field<T>`, `HasMany<T>`, `BelongsTo<T>`, `HasOne<T>` typed
  relation declarations
- `include()` for eager loading
- Compile-time mapping from ORM declarations to `MappingSpec<T>`
  and `Relation<A,B>` metadata
- `find_by`, `where`, `order_by`, `count` etc. as static methods
  on Record subclasses

Outcome: Rails/Django/Hibernate users get the model layer they
expect, with static typing they may not realize they're getting,
and no proxy-object overhead.

### V4 — Compiler-integrated surfaces

Land:

- Mad-C compiler frontend integration with Gecko
- Native `sql::`, `cypher::`, `gql::` sub-grammars
- Lowering passes from sub-grammar ASTs to Query IR
- Symbol resolution and type checking in query expressions
- `using namespace sql;` / `using namespace gql;` activation
- Ruby-shaped grammar mode (`.rmad` files, full do/end syntax,
  trailing block recognition)
- Native Objective-C bracket syntax (replaces V2 macros)
- Runtime parsers (`parse_sql`, `parse_cypher`, `parse_gql`) for
  dynamic queries

Outcome: native query syntax, native bracket dispatch, native Ruby
grammar all available. Users writing Mad-C in any of these styles
get full compiler support — type checking, IDE autocomplete, error
messages.

### V5 — Python-shaped grammar

Land:

- Python-shaped grammar mode (`.pymad` files)
- Indentation-significant lexer
- List/dict/set comprehensions and generator expressions
- `for ... in ...` over datasets and cursors
- `len`, `range`, `enumerate`, `zip`, `sorted` as Python idioms
  bridging to Mad-C operations
- Python-style class declarations lowering to Mad-C structs (with
  appropriate type discipline)

Outcome: Python developers can write recognizable Python-shaped
code that compiles to native Mad-C with full type checking.

### V6+ — User-extensible surfaces

Land:

- Public API for registering user-defined surfaces
- Documentation for writing a Gecko sub-grammar plus a lowering
  pass
- Example third-party surface (Datalog-style, or APL-style array
  operations, or domain-specific)
- Tooling support: an LSP plugin that knows about registered
  surfaces

Outcome: the multi-surface architecture becomes open — users can
add domain-specific query vocabularies (regex DSL, time-series
query language, scientific notation) that fit Mad-C's substrate
without modifying the compiler.

## Discipline for adding surfaces

Adding a new surface to Mad-C requires:

1. **Define what operations the surface expresses.** Is it
   filter/map/reduce? Graph traversal? Schema declaration?
   Aggregation? The substrate already supports most operations;
   the surface is a way to *write* them.

2. **Choose a namespace prefix.** Following Mad-C convention,
   surfaces use namespace prefixes for explicit invocation.
   `madc::xxx::` for built-in surfaces, third-party choice for
   user surfaces.

3. **Decide library-only vs compiler-integrated.** Can it be
   expressed with C++ templates and operator overloading? Or does
   it need grammar work? Library-only is simpler and lands
   sooner; compiler-integrated provides better ergonomics for
   complex syntactic patterns.

4. **Map the surface's vocabulary to Query IR.** Each operation
   in the surface should have a clear lowering to one or more
   substrate operations. Surfaces that can't fully lower to the
   IR are not ready to be surfaces — they're hints that the IR
   needs to grow first.

5. **Document explicitly what's supported and what's not.** A
   surface that supports 80% of its inspiration language's
   features should be honest about which 20% is missing. The
   `python::` surface is "Python-shaped Mad-C," not "Python."

6. **Provide `.explain()` or equivalent.** Users should always
   be able to see what the surface compiled to.

This discipline keeps the surfaces architecturally honest: each
one adds vocabulary without changing semantics.

## Open Questions

- How aggressive should the default surface be in unqualified
  Mad-C code? Currently the C-native API is the default; should
  the LINQ-style chaining methods also be unqualified, or require
  `using namespace madc::linq;`?
- For Ruby-shaped and Python-shaped grammar modes, how do they
  interact with the textual query languages? Can a Ruby-shaped
  file contain `sql::SELECT ...` expressions? (Probably yes —
  query languages are namespaced and grammar-mode-independent.)
- Should there be a "polyglot mode" where multiple surfaces are
  active simultaneously with name-resolution rules? Or is
  one-surface-at-a-time cleaner?
- How do third-party surfaces handle versioning? If two registered
  surfaces use conflicting keywords, what's the resolution rule?
- Should ORM relations participate in the derivation pattern (a
  `Record<T>` could declare a `Derived<S>` keyframe relation)?
  Probably yes, but the syntax needs design.
- What's the right testing discipline for cross-surface
  interoperability? Probably a test matrix where each pair of
  surfaces is exercised with shared data.

## Relationship to the other plans

This plan describes vocabularies that operate on:

- `madcdis` substrate (datasets, relations, query IR, planner,
  in-memory drivers)
- `madcdat` external drivers (when they're loaded, the same
  surfaces transparently extend over external data)

The surfaces don't modify the substrate or the driver layer; they
provide alternate ways to express operations against them.

Compiler integration (V4-V5) is shared work with `madcdis-plan.md`'s
V4 phase (native query syntax). The Mad-C compiler grows
support for sub-grammars and lowering passes once; both query
languages and grammar-shaped modes benefit.

See `madcdis-plan.md` for the substrate that surfaces compile to.
See `madcdat-plan.md` for the external drivers that surfaces
transparently extend over.
