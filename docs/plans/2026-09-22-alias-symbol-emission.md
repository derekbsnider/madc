# `__attribute__((alias))` symbol emission — handoff plan (2026-09-22)

**Status:** diagnosed, designed, NOT implemented. Branch
`feature/headerless-win-weak-import-claude` (carries only
`scripts/promote_release.sh` so far).

**Why it matters:** this is the gap between madc *compiling* all five MIR
translation units (done, v0.100.1) and *linking* a madc-built libmir — i.e.
between "compiles its own backend" and self-hosting.

---

## 1. What is actually broken

madc never emits a symbol for `__attribute__((alias("target")))`. Not for the
eight MIR exports specifically — for the attribute at all.

Reducer (`tmp/alias2.c` shape), **no asm label involved**:

```c
int real_fn(int x) { return x + 1; }
int real_var = 42;
extern int plain_fn_alias(int)  __attribute__((alias("real_fn")));
extern int plain_var_alias      __attribute__((alias("real_var")));
```

| compiler | symbols emitted |
|---|---|
| gcc `-std=gnu17` | `T real_fn` · `T plain_fn_alias` · `D real_var` · `D plain_var_alias` |
| clang `-std=gnu17` | identical to gcc |
| **madc** | `T real_fn` · `D real_var` — **aliases absent** |
| **c2m `-fobject`** | `T real_fn` · `D real_var` — **aliases absent** |

## 2. c2mir is NOT the reference here (measured 2026-09-22)

Checked because the engine's own behaviour should normally be the guide. It
cannot be, for this feature:

- `c2m -fobject -o x.o` on the plain-alias reducer emits **2** symbols. The
  alias attribute is silently ignored — no diagnostic, no symbol.
- The real MIR construct is a **syntax error** in c2m:
  `tmp/alias.c:2:40: syntax error on identifier (expected '<declarator>')`
  — c2m rejects both `__typeof` and an `asm` label in that declarator position.

So madc is AHEAD of c2mir here (it parses both forms), and **gcc/clang remain
the oracle**. This is consistent with how the exports are used: `libmir` is
built by gcc/clang, and its `asm`-label aliases exist so that an object emitted
by madc (or c2m) referencing `mir.va_arg` resolves against that gcc-built
library. Note `c2m -c` means "binary MIR"; native ELF needs `-fobject` and NO
`-c`. Getting that wrong yields a 0-byte or `data` file and looks like a crash.

## 3. Root cause — one field carries two different facts

`Variable::storage_alias_name` is written from BOTH:

- `Program::consume_gnu_asm_label` (`src/parser.cpp:1852`) — the **asm label**,
  i.e. the symbol name this declaration should be EMITTED under;
- the GNU attribute scanner (`src/parser.cpp:1793`) — the
  `__attribute__((alias("F")))` **target**, i.e. the symbol this declaration is
  an alias OF.

They share one out-parameter and one destination field. The field's own comment
in `src/cir_builder.cpp:293` admits the overload. For MIR's construct, which
carries both, one silently wins.

Worse, madc consumes the field in the **opposite direction from gcc**: it makes
*references to the alias* resolve to the target's storage (a REDIRECT), rather
than *defining a second symbol* at the target's address (a DEFINITION). The
redirect is genuinely needed and must not be broken — it is how system-header
class statics bind to their real Itanium symbols (`<compare>`'s
`strong_ordering::less`; see `cir_builder.cpp:11347`, `:16790`). The DEFINING
half was simply never built.

## 4. The fix needs no MIR change

MIR already exposes both halves of the primitive (`third_party/mir/mir-debug.h`):

```c
int MIR_object_find_symbol (MIR_object_t obj, const char *name,
                            int *sec, uint64_t *value, uint64_t *size);   /* :238 */
int MIR_object_add_symbol  (MIR_object_t obj, const char *name, int sec,
                            uint64_t value, uint64_t size,
                            int func_p, int local_p, int weak_p);         /* :219 */
```

An alias is exactly "another name at the target's section + value". Tier 1
(lower/resolve in madc) per `.claude/rules/lowering-vs-raising.md` — no fork
divergence.

## 5. Steps

1. **Separate the two facts.** Add a field beside `storage_alias_name` (e.g.
   `Variable::alias_definition_target`, `include/madc.h:3259` /
   `include/datatokens.h:105`) holding ONLY the `__attribute__((alias))`
   target. Leave `storage_alias_name` meaning exactly what it means today.
   Give `consume_gnu_asm_label` and the attribute scanner separate
   destinations — today they share one out-param.
   ⚠️ Check the freeze format: `include/madc.h:5755` notes
   `storage_alias_name` is serialized (v39). A new field needs a version bump
   or must be recomputed on thaw.
2. **Emit the alias symbols post-link.** Model on `cir_register_tu_init`
   (`src/madc_cir.cpp:1053`) — same shape, same `MIR_gen_get_object(ctx)`
   handle, called from the same place. For each file-scope Variable with a
   non-empty `alias_definition_target` whose target is DEFINED in this TU:
   `MIR_object_find_symbol(target)` then `MIR_object_add_symbol` with
   `var_emit_name(v)` (which already honours the asm label),
   `func_p` = target is a function, `local_p=0`, `weak_p` = the decl is weak.
   A target that is not defined in this TU is NOT an error — gcc requires the
   target be defined in the same TU, so diagnose it the way gcc does
   (`alias must be defined in the same translation unit`) rather than silently
   dropping it.
3. **Test** `tests/testasmlabelalias.mad` (+ `.flags` `--std=c17`): both forms
   (alias alone; alias + asm label), function and data, with the oracle taken
   from gcc AND clang. Assert through `nm` on a `-c` object, not just runtime
   behaviour — the whole point is the symbol table. NOTE the existing
   `tests/testasmlabelmirbuiltin.darwin_skip` precedent if darwin differs.
4. **Verify the real thing:** the eight exports should then appear.
   Per-TU counts from UPDATE 77: `c2mir.c` gcc 47 globals / madc 41 (missing
   the six `__mir_*oti` int128 helpers, `mir-int128-helper.h:302-315`);
   `mir.c` gcc 151 / madc 149 (missing `mir.va_arg`, `mir.va_block_arg`,
   `mir-x86_64.c:154,156`). Re-diff after the fix.
5. **Then try to LINK a madc-built libmir** — the actual milestone this serves.

## 6. Traps

- `--emit=c11` MANGLES asm labels (`probe asm("mir.arg_memcpy")` emits as
  `extern char mir_x2earg_memcpy;`), so emit-C is NOT a valid oracle for
  anything involving asm labels. Verify with `nm` on a real object.
- Darwin strips one leading underscore from asm labels at parse
  (`parser.cpp:1858`, `MADC_TARGET_APPLE_P`). The Mach-O writer re-prepends it.
  Do not double-strip when emitting the alias symbol.
- `MIR_object_add_symbol` never dedupes by name (`madc_cir.cpp:620`). Emitting
  the same alias twice yields two symtab entries.
