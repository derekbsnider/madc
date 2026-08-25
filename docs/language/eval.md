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
// rows: { kind, name, line, column } for the buffer's OWN definitions,
// source order (kind: "function" today; classes/globals are a named
// extension seat)
```

`filename` is the display name diagnostics carry (default `<source>`).
Positions match what a file-based compile of the same text reports.

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
