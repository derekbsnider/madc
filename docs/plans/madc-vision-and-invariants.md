# madc — Vision & Architecture Invariants (the north star)

**Purpose:** crystallize the long-term vision and the **invariants** every change must
satisfy, so that *nothing we build blocks the vision and everything we build enables
it*. This governs the roadmap (`docs/plans/cpp-support.md`) and every language task.
Before merging any change, run the **"does this block the vision?" checklist** at the
bottom.

---

## The vision

**madc is, ultimately, a polyglot transpiler:** read source written in language/standard
**X**, emit target **Y**, through **one intermediate representation**. The C/C++ dialect
work happening now is the first, concrete instance of a general capability.

Three layers make it possible (all keyed off **one** `LanguageStd` enum):

1. **One IR — `cir_node` / MC11-IR** (`docs/rules/mc11-ir.md`): a C/C++ AST that is
   *deliberately BOTH* lowered (derives from c2mir `node_t`) **and** high-level
   (carries originating tokens + parse subtree + file/line/col). Single source of
   truth for execution AND for every render target.
2. **The `--std=` / `LanguageStd` enum on BOTH ends** (`include/madc.h`; default
   `STD_MADC`, the permissive superset): selects the **input** dialect/language (what
   we parse + which keywords/features are active) AND the **output** target (what we
   emit / the c2mir target std). One enum, two axes.
3. **A keyword/feature/syntax → standard/language REGISTRY** (the keystone, roadmap
   P2.11): declarative metadata `{introduced_in, removed/deprecated_in, dialect/language,
   version}`. Makes gating, target-selection, and conversion all *one lookup* — data,
   not scattered `if (is_c_mode())` branches.

From those three, the capabilities fall out: **input gating** (P2.11), **c2mir
backend-target** selection (P2.12), **standards conversion** (P2.13, input-std ≠
output-std), and the far-future **polyglot transpile** (input axis generalized from
"C dialect" to "source language").

## Why `cir_node` (a C/C++ AST) is the RIGHT universal IR

**Most languages' reference implementations are themselves written in C or C++** —
CPython (C), Ruby MRI (C), PHP (C), Perl (C), Lua (C), V8/Node (C++), … So every such
language's semantics are *already* expressed in C/C++ by its own implementation: a
faithful C/C++ AST is the **common denominator / lingua-franca substrate** that all of
them reduce to. cir_node-as-IR is therefore *principled*, not C-centric accident.
Corollary — the **lowering canon**: to lower a construct from language L, do what L's
own reference C/C++ implementation does (the polyglot analogue of "gcc is canon").

---

## The invariants — every change must satisfy these

Violating one of these is, by definition, a blocker to the vision. They override
convenience.

- **I1 — Dual-view IR is sacred.** `cir_node` stays BOTH lowered (for c2mir) AND
  high-level (originating tokens/parse subtree/positions). Never reconstruct high-level
  structure from emitted comments/pragmas; never introduce a competing parallel AST.
  (`docs/rules/mc11-ir.md`.)
- **I2 — The C/C++ floor is faithful and universal.** Everything lowers to constructs
  expressible in C/C++ (the substrate every language reduces to). Don't admit IR
  constructs that can't map to C/C++. New features get a **C11 lowering** (or a fork
  raise per lowering-vs-raising), not a bespoke non-C path.
- **I3 — Never hardcode a standard or target; reference the enum.** Both the input
  dialect/language AND the output/c2mir-target std are `LanguageStd` values. No baked-in
  "C11", no binary "is C++" assumptions in lowering. (Roadmap P2.12 de-hardcodes the
  current C11 assumption.)
- **I4 — Every feature/keyword/syntax earns a REGISTRY entry.** A new construct MUST be
  registered with its `{introduced/removed standard-or-language, dialect}` and gated by
  it — keyword AND feature dispatch. No always-on C++ features; no scattered
  `is_c_mode()` specials. This is what keeps gating + conversion + targeting data-driven
  and what lets the input axis generalize to other languages. (Roadmap P2.11.)
- **I5 — Render targets share the one IR + enum.** All emit targets (C11, MC11, C++,
  madc, … and future languages) read the single IR, selected by the shared enum — one
  emitter, never parallel rendering paths.
- **I6 — No special-casing, no shortcuts, fix at the deepest layer.** Special-cases are
  future blockers to generalization (the retired `dtSTRING`/`ns_stl` shortcuts are the
  cautionary tale). Model the real abstraction; one machinery, the only fork being
  precompiled (`emit_symbol`) vs madc-compiled.
- **I7 — Borrowing lowers to the C/C++ AST.** Borrowed-language support (functions today
  via `php::`/`perl::`/… namespaces calling those languages' C impls; syntax/features
  tomorrow) lowers to the same C/C++ AST. Keep the borrowing/namespace mechanism general,
  not per-language-special.
- **I8 — Keep the standards model honest.** Gating must actually reject/ignore
  out-of-dialect constructs (`--std=c11` must not silently accept C++-only syntax). "C
  still works" = "C++ features correctly gated OFF in C mode."

## The "does this block the vision?" checklist (run before every change)

1. Does it **hardcode** a standard, dialect, or target (instead of referencing the
   enum)? → blocker (I3).
2. Does it add a keyword/feature/syntax **without a registry entry + std/language gate**?
   → blocker (I4).
3. Does it **special-case** one type/dialect/language in a way that won't generalize? →
   blocker (I6).
4. Does it introduce an IR construct **not expressible in C/C++**, or break the
   dual-view? → blocker (I1, I2).
5. Does it add a **parallel** AST or render path instead of using the one IR/enum? →
   blocker (I5).
6. Does it make C mode **silently accept** out-of-dialect constructs? → blocker (I8).

If any answer is "yes," redesign so the answer is "no." A change that passes all six is,
by construction, *enabling* the vision rather than blocking it.

---

## Status of the machinery (what exists vs target)

- **EXISTS:** the one IR (`cir_node`); the `LanguageStd` enum (`madc.h:1005`, default
  `STD_MADC`) + `--std=` parsing; partial input gating (lexer.cpp:1309-1339 behind
  `!is_c_mode()` — `class`/`new`/`namespace`/`try`/`template` are C++-only); `--emit=c11`
  / `--emit=mc11` render targets; borrowed-language function namespaces; the C++→C
  lowering (Cfront-style — already a standards conversion).
- **TARGET (roadmap):** the declarative registry (P2.11) replacing ad-hoc gating;
  enum-driven c2mir target (P2.12); explicit standards conversion (P2.13); the polyglot
  generalization (P3). The C-stability pass implements the gating/registry honestly.

## References
- `docs/plans/cpp-support.md` — the C++23/C23 compliance roadmap (principle #6 + P2.11–
  P2.13 + the P3 polyglot note) this vision governs.
- `docs/rules/mc11-ir.md` — the SET-IN-STONE dual-view IR definition.
- `docs/adr/0001-cir-c2mir-backend.md` — the C-all-the-way-down backend decision.
- Memory: `project_madc_vision`, `project_std_enum_gatekeeping`, `project_north_star_c23_cpp23`.
