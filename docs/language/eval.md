# Runtime Eval — `madc::eval_*`

A madc program can compile and run madc source *at runtime* — the full
compiler is in the room. The API lives in the `<ns_madc>` embedded
header (auto-included on first `madc::` use in the default dialect;
standards modes need the explicit include). Under the `madc` CLI the
whole surface is enabled; embedding hosts can restrict it — see
[Security](#security).

Two families:

- **Expression eval** — one expression, evaluated against optional
  context: `eval_expression_*`
- **Unit eval** — a full program fragment with declarations, control
  flow, and includes: `eval_*` / `eval_unit`

The primary API is **value-first**: sources are `const char *`, results
land in `madc::value` destinations (string-kind, carrying the rendered
text), and context keys are `const char *` — so an eval-using script
never needs `<string>`. The `std::string` overloads shown in some
examples are *conveniences*, declared only when `<string>` is included
**before** `<ns_madc>` (auto-include orders them correctly in the
default dialect; explicit includers write `#include <string>` first).

## Expression Eval

Typed result forms, plus out-parameter overloads that pick the
overload from the destination's type:

```c
#include <iostream>
#include <string>
#include <ns_madc>
using namespace std;

int main()
{
    cout << madc::eval_expression_int("6 * 7") << endl;      // 42
    cout << madc::eval_expression_bool("6 > 3") << endl;     // 1
    cout << madc::eval_expression_double("1.5 * 2.0") << endl; // 3

    long n = 0;
    madc::eval_expression(n, "40 + 2");     // long& overload
    cout << n << endl;                      // 42

    string rendered;
    madc::eval_expression(rendered, "sqrt(9.0) + cos(0.0)");
    cout << rendered << endl;               // 4.000000
    return 0;
}
```

The `string`-destination overload *renders* any result type ("42",
"4.000000", "echo"); `eval_expression_string` is the strict
string-typed coercion instead.

## Call-Site Scope Capture

An evaluated expression sees the caller's locals by name:

```c
#include <iostream>
#include <string>
#include <ns_madc>
using namespace std;

int bonus_roll(int base)
{
    int bonus = 2;
    return madc::eval_expression_int("base + bonus");
}

int main()
{
    cout << bonus_roll(40) << endl;   // 42
    string who = "echo";
    string out;
    madc::eval_expression_string(out, "who");
    cout << out << endl;              // echo
    return 0;
}
```

## Unit Eval

The unit family compiles a full fragment. Bare statements are wrapped
into an `__madc_eval()` entry point automatically; a fragment may also
define `__madc_eval()` itself, alongside helper functions and
`#include`s:

```c
#include <iostream>
#include <string>
#include <ns_madc>
using namespace std;

int main()
{
    cout << madc::eval_int("return 40 + 2;") << endl;   // 42

    string src =
        "int helper() { return 40; }\n"
        "int __madc_eval() { return helper() + 2; }\n";
    string rendered;
    madc::eval_unit(rendered, src);
    cout << rendered << endl;                           // 42

    string src_double =
        "#include <math.h>\n"
        "return sqrt(9.0) + cos(0.0);\n";
    cout << madc::eval_double(src_double) << endl;      // 4
    return 0;
}
```

`eval_bool` / `eval_string` complete the typed set; `eval_unit`
renders whatever `__madc_eval()` returns into its out-string.

## Context Objects

The `_ctx` forms evaluate against an explicit context you build with
the `context_set_*` helpers — entries nest, and dotted paths reach
into them:

```c
#include <iostream>
#include <string>
#include <ns_madc>
using namespace std;

int main()
{
    array ctx;
    array user;
    string name_key = "name";
    string level_key = "level";
    string user_key = "user";

    madc::context_set_string(user, name_key, "echo");
    madc::context_set_int(user, level_key, 41);
    madc::context_set_array(ctx, user_key, user);

    cout << madc::eval_expression_int_ctx("user.level + 1", ctx) << endl;        // 42
    cout << madc::eval_expression_bool_ctx("user.name == \"echo\"", ctx) << endl; // 1
    return 0;
}
```

`context_set_real` adds doubles; every expression and unit form has a
`_ctx` twin (`eval_int_ctx`, `eval_expression_double_ctx`, …).

## Compiler Data (diagnostics and outline)

The compiler's own structured data, as values — compile (NEVER execute)
a source buffer with the same front end `madc` runs, in a
policy-clamped child, and read what it found. Diagnostics are CAPTURED,
not printed (nothing reaches stderr); nothing in the buffer runs. This
is the meta-level surface an IDE projects (`tools/madcide`'s
diagnostics pane and outline), and it serves any tool that wants
compile results as data — a linter, a doc generator, a test harness.

```c
value d;
madc::diagnostics(d, source, "buffer.mad");
// rows: { severity, phase, message, file, line, column }
// severity "error"/"warning"; an empty array = a clean buffer

value o;
madc::outline(o, source, "buffer.mad");
// rows: { kind, name, line, column, end_line } for the buffer's OWN
// definitions, source order (kind: "function" today; classes/globals
// are a named extension seat; end_line = the closing brace's line)
```

`filename` is the display name diagnostics carry (default `<source>`).
Positions match what a file-based compile of the same text reports.

### The render query (`madc::emit`)

The same child, one step further: parse the buffer and render its
`cir_node` tree (MC11-IR) as a target language — the `--emit=`
vocabulary, byte-identical to what `madc --emit=<target> file` prints.
This is what feeds an editor's code views (`tools/madcide`'s `^K N`).

```c
value text;
if ( madc::emit(text, source, "buffer.mad", "c11") )   // or "mc11", "c++"
    print("{}", text);
// false = unknown target, or a buffer that does not parse/translate
// (diagnostics captured, never printed); nothing in the buffer runs
```

The `"c++"` target is the REVERSE render: the TU's retained source — its
own `#include` directives as written, then the token echo (trivia
preserved; string escapes re-escaped so the render re-lexes). For a
C/C++ buffer the render recompiles under g++/clang++ and behaves
identically (`scripts/emitcxx_roundtrip_gate.sh` pins it in fulltest).
madc-dialect constructs pass through unrespelled — cross-language
respelling is a named future seat. Macro uses echo expanded; numeric
literals canonicalize where the original text was not retained.

### Persistent parse handles

The same compiler-data machinery given a LIFETIME (madcide AST-1): a
handle owns a live parse per TU, so outline / diagnostics / the
enclosing-definition query answer from RETAINED state — no re-parse per
query. Refresh is a whole-TU re-parse (an IDE refreshes on check/save;
composition reads what the last refresh retained).

```c
long h = madc::parse_open(source, "buffer.mad");  // >= 1; a buffer with
                                                  // errors still opens —
                                                  // its state IS the rows
madc::parse_refresh(h, new_source);               // whole-TU re-parse
value o, d, e;
madc::parse_outline(o, h);                        // the outline rows above
madc::parse_check(d, h);                          // the diagnostics rows
madc::parse_enclosing(e, h, line, column);
// the INNERMOST of the TU's own function definitions containing
// (line, column) — { kind, name, line, column, end_line }, or an empty
// value when none (madcide's status line)
value hl;
madc::parse_spans(hl, h);
// highlight classification rows for the TU's OWN tokens:
// { line, column, length, class } — 1-based line, column = the span's
// START (0-based); classes: keyword, ident, number, string, comment
// (handles retain trivia — the IDE's fidelity mode), type, function
// (an identifier the tree defines as a function, on its head line).
// Data, not styling: a theme (app data) maps class names to colours.
madc::parse_close(h);                             // handles never reuse

long hf = madc::parse_open_file("src/tu.c");      // the lexer's own file
                                                  // ingestion; relative
                                                  // #includes resolve as
                                                  // the CLI's do; 0 =
                                                  // unreadable path
```

A project handle groups a `compile_commands.json` manifest's TUs and
parses every one on open, each with its OWN manifest options
(-I/-D/--std, the `.c` → gnu17 default) — project diagnostics match the
`--project` build:

```c
long p = madc::project_open("proj.cc.json");      // 0 = unreadable manifest
value tus;
madc::project_tus(tus, p);                        // rows: { file, handle }
                                                  // (handle 0 = that file
                                                  // unreadable / options
                                                  // refused)
madc::project_close(p);                           // closes its TU handles
```

Thread contract: a handle is confined to the thread/program that opened
it — the runtime-eval machinery's confinement.

## Security

Embedding hosts control the whole surface: full-unit eval is gated by
the engine's runtime-eval policy, function calls inside expressions by
the expression policy, and call-site scope capture by the scope-access
hooks. A host that grants none of them still gets pure-arithmetic
expression eval. The `madc` CLI enables everything. The compiler-data
publics run under the same runtime-eval child policy.

## Files

- `include/madc/ns_madc` — the declaration-only script surface
  (also home of [`madc::sys`](sys-object.md))
- `src/ns_madc.cpp` — the host-side implementations
- `tests/testmadceval*.mad` — the gates these examples are modeled on
