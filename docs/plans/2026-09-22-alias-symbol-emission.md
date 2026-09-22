# `__attribute__((alias))` symbol emission — handoff plan (2026-09-22)

**Status: DONE 2026-09-22** — implemented, gated, and the milestone it serves
is reached. Commit `4d497614b` on
`feature/headerless-win-weak-import-claude`. §7 records what was measured
afterwards, including the ONE defect still between here and a fully
madc-built `c2m`. The design below is what shipped, with two corrections
noted in §7.

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


---

## 7. What shipped, and what it found (2026-09-22)

### Two corrections to the design above

- **Step 1 needed TWO new fields, not one.** `storage_alias_name` answers "what
  does a REFERENCE to this declaration resolve to". The alias's own emitted
  name is a third fact: the asm label when there is one, the declared name
  otherwise. So `Variable` now carries `asm_label` and
  `alias_definition_target` beside it, each with one writer.
- **`var_emit_name(v)` is the WRONG name to emit the alias under** (step 2 said
  to use it). For a data alias it returns the TARGET — that is the redirect. The
  alias symbol's name is `v.asm_label.empty() ? v.name : v.asm_label`.
- **It did need one MIR change after all** — a small one, and not a semantic
  raise. A DATA alias's target is only a DEFINED symbol once the capture's
  module-data walk has placed it, and that walk ran *inside* the emit entry,
  i.e. after every chance to annotate. `MIR_gen_object_prepare` (mir-gen.c,
  mir-gen.h) exposes the existing one-shot walk; the emit entries still run it
  for themselves. madc calls it only on a TU that actually declares an alias,
  so no other emit ordering moves. Upstreamable as-is.

### Gates

- `tests/testasmlabelalias.mad` (+ `.flags` `--std=c17`, `.expect`) — the
  REFERENCE half, green in JIT, `--exe` and `--obj`. Oracle: `gcc -std=gnu17`
  and `clang -std=gnu17` both print the three asserted lines.
- `scripts/check-alias-symbol-emission.sh` — the DEFINING half, `nm` against
  the gcc oracle, wired into `fulltest` after
  `check-var-emit-name-bypass.sh`. Two negative controls: the comparison must
  notice a missing alias name, and the asm label must vanish from an object
  built without the attribute.

### Step 4 — the eight exports, re-diffed

`nm -g --defined-only`, madc vs the gcc-built object, per TU. **Zero gcc-only
symbols in all four libmir TUs** (madc's extra entries are its own
`__madc_shim_*` thunks):

| TU | gcc globals | madc | gcc-only |
|---|---|---|---|
| `mir.c` | 151 | 153 | **0** (was missing `mir.va_arg`, `mir.va_block_arg`) |
| `c2mir/c2mir.c` | 47 | 49 | **0** (was missing the six `__mir_*oti`) |
| `mir-gen.c` | 19 | 20 | **0** (`mir.arg_memcpy`, `mir.ld2i`, `mir.ui2d`, `mir.ui2f`, `mir.ui2ld` all present) |
| `mir-debug.c` | 40 | 40 | **0** |

### Step 5 — LINK a madc-built libmir: DONE, AND IT WORKS

`ar rcs libmir_madc.a` over madc-built `mir.o mir-gen.o mir-debug.o
mir-debug-gdb.o c2mir.o`, linked into a `c2m`. Object-swap bisection:

| c2mir.o | libmir | `c2m … -eg` |
|---|---|---|
| gcc | **madc** | **clean, rc=0, correct output** |
| madc | gcc | correct output, then SIGSEGV in `c2mir_finish` |
| madc | madc | correct output, then SIGSEGV in `c2mir_finish` |

**A libmir compiled entirely by madc is correct** — front end, JIT generator
and teardown. That is the milestone this plan served.

Two things are needed to link it at all and are not defects: the madc objects
reference madc's own value shims (`madc_value_get_type_id` and friends), so the
link carries `-lmadc` (this is what `-static-libmadc` / the AOT ledger is for);
and `/usr/local/lib/libmadc.so.0` is an ANCIENT copy (it still links
`libasmjit.so`) that the loader prefers — set `LD_LIBRARY_PATH` to the repo
`lib/` or the binary dies with `undefined symbol: madc_value_get_type_id`.

### The two defects this uncovered — NEXT on the self-hosting path

1. **madc's compilation of `c2mir.c` crashes in `c2mir_finish` after any
   GENERATOR run.** Reproduce with the mixed binary above (madc `c2mir.o` + the
   gcc libmir, so nothing else is in question). `-ei` clean; `-c` clean;
   `-eg` and `-el` both print the program's correct output and then SIGSEGV in
   teardown. `c2mir_finish` is four calls — `str_finish`, `reg_pages_finish`,
   `MIR_free`, `c2m_dbg_reset` — so the reducer is already small. The output
   is only visible under `stdbuf -o0`: buffered stdout is lost to the signal,
   which makes this look like "produced nothing" if you do not force it.
2. **`c2mir/c2mir-driver.c:1069` — "excess elements in array/struct/union
   initializer" (3 check errors).** A SIXTH MIR TU madc cannot compile; it was
   never in the "all five TUs" count because it is `c2m`'s `main`, not part of
   libmir. Wanted for a fully madc-built `c2m`.
