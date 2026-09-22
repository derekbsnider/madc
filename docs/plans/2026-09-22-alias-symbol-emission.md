# `__attribute__((alias))` symbol emission — handoff plan (2026-09-22)

**Status: DONE 2026-09-22**, and so are both defects it uncovered. Commits
`4d497614b` (alias emission), the union-designator fix and the `void **`
pointer-scaling fix, on `feature/headerless-win-weak-import-claude`. §7 records
the alias work; **§8 records the two follow-on fixes and the 🏁 fully
madc-built `c2m`**. The design below is what shipped, with corrections noted
in §7.

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


---

## 8. The two defects §7 banked — both FIXED, and the milestone completed

The owner's rule applies: a feature is not done, and no full suite runs, while
known defects of it are open. Both are fixed, each in its own commit with its
own reducer and both oracles.

### 8.1 A union brace initializes ONE member, named by a designator

C11 6.7.9p17: a union's brace initializer initializes exactly one member, and a
bare positional list can only ever name the FIRST. madc lowered every
designated initializer to a positional list with the earlier slots zero-filled
— correct for a struct, where `{.z=3}` and `{0,0,3}` are the same object, and
impossible for a union: `{.a = p}` became a TWO-element union initializer and
c2mir refused it, *"excess elements in array/struct/union initializer"*.

`c2mir-driver.c:1067-1069` passes three `(MIR_val_t){.i = …}` / `{.a = …}`
compound literals to `MIR_interp`, and `MIR_val_t`'s first member is
`MIR_insn_code_t ic` — so every one of them designates a non-first member. That
is why the sixth TU would not compile.

Nothing was lost at parse: `parse_compound_struct_lit` writes the value into
that member's positional SLOT, so the slot index IS the member index. It only
had to be spelled back as the `N_FIELD_ID` designator c2mir's grammar already
takes. The three sites that built an aggregate's initializer list are now one
owner, `CirBuilder::aggregate_init_list`, which carries a `slots_are_elements`
flag because **madc types a fixed array as its ELEMENT type** with the count on
the Variable — without it, `val_t a[2] = {{.i=1},{.i=2}}` read its two
ELEMENTS as two of `val_t`'s MEMBERS. `--emit=c11` learned to spell `.name`; it
had been printing `/*<unhandled FIELD_ID>*/`.

Gate: `tests/testuniondesignatedinit.mad`. Every shape designates a NON-first
member, because the first member is exactly the case the positional lowering
already got right.

### 8.2 `void **` is not a size-1 pointer — and that was the `c2mir_finish` crash

The teardown crash was not a teardown bug. GNU C allows arithmetic on a
`void *` with element size 1; c2mir refuses it, so madc rewrites such an
operand to `(char *)` before the `+`. `is_size1_pointer` decided with
`p->base_type->rawtype() == dtVOID` — and `DataDefPTR::rawtype()` reports what a
pointer chain ultimately points AT (`datadef.h:1763`, *"T\*\* recurses to the
innermost scalar"*). So it said yes to `void **`, `void ***` and deeper, whose
pointee is a complete 8-byte object, and **their arithmetic scaled by one
byte**.

c2mir's own context accessor is:

```c
static inline c2m_ctx_t *c2m_ctx_loc (MIR_context_t ctx) {
  return (c2m_ctx_t *) ((void **) ctx + 1);
}
```

madc emitted `lea 0x1(%rdi)` for it. A madc-built c2mir therefore **stored its
context at ctx+8 and looked for it at ctx+1** — everything worked until
teardown dereferenced NULL.

It was silent: `p[1]` indexing never goes through the rewrite and was always
right, so only the explicit `p + 1` form read a pointer one byte out of place
and produced garbage rather than a fault. `DataDef::is_cstr()` already carried
the identical guard one type over (*"!is_pointer() excludes char\*\*"*); the
predicate is now spelled the same way.

Gate: `tests/testvoidptrptrarith.mad`, with the GNU `void *` size-1 line as a
control that must NOT move.

### 8.3 🏁 A `c2m` built ENTIRELY by madc

All SIX MIR translation units compiled by madc — `mir.c`, `mir-gen.c`,
`mir-debug.c`, `mir-debug-gdb.c`, `c2mir/c2mir.c`, `c2mir/c2mir-driver.c` —
archived into `libmir.a` and linked into a `c2m`. It compiles C, JIT-generates
machine code and runs it, in `-ei`, `-eg` and `-el`, and the `.bmir` it emits
runs under the gcc-built `c2m`.

Differentially tested against the gcc-built `c2m` over 140 `c-tests` programs:
**137 identical**.

### 8.5 The three that differ — DIAGNOSED to ONE root cause (see below); §8.4 kept as first-pass notes

### 8.4 The three that differ — first-pass notes

All three are float / long-double conversion. **madc itself** (embedding a
gcc-built libmir) runs all three correctly, so madc's own float handling is
fine — it is madc's COMPILATION of MIR's conversion code that is wrong.

| program | where | symptom |
|---|---|---|
| `lacc/convert-int-float.c` | madc-built **libmir** | SEGFAULT |
| `lacc/convert-unsigned-float.c` | madc-built **libmir** | SEGFAULT |
| `lacc/bitfield-types-init.c` | both mixes | wrong long-double BYTES (gcc `0,-99,20,-54,77,86,0,0` vs madc `0,-108,116,17,-4,127,0,0`) |

Located by OBJECT SWAP, which is the method to reuse: build three binaries —
all-madc, madc `c2mir.o` + gcc libmir, gcc `c2mir.o` + madc libmir — and diff
each against `obj/mir/host/c2m`. Two link commands name the TU.

⚠️ Redirect the output and check `rc`: a segfault AFTER correct output reads as
"produced nothing", because stdout is still buffered. Use `stdbuf -o0`.


---

## 9. The last item: a MIR BOOTSTRAP CYCLE — DIAGNOSED AND FIXED 2026-09-22

### 9.1 One of the three was never a defect

`lacc/bitfield-types-init.c` differs with **every** one-object swap, including
`mir-debug-gdb.o`, which cannot touch long-double arithmetic. The negative
control settles it: an **all-gcc** `c2m` linked the same way (`-lmadc`, g++)
also differs — and prints a *third* byte pattern. The program dumps the padding
bytes of an uninitialized `long double`. Not a madc defect.

**Real score: 138/140.**

### 9.2 The other two are one root cause

x86-64 has no unsigned-64→float instruction, so MIR's generator rewrites
`MIR_UI2F` / `MIR_UI2D` / `MIR_UI2LD` / `MIR_LD2I` into a **call to its own
builtin** (`get_builtin`, `mir-gen-x86_64.c:800`). Those builtins are four
one-line static functions in that same file (`:757-760`):

```c
static float       mir_ui2f  (uint64_t i)    { return (float) i; }
static double      mir_ui2d  (uint64_t i)    { return (double) i; }
static long double mir_ui2ld (uint64_t i)    { return (long double) i; }
static int64_t     mir_ld2i  (long double ld){ return (int64_t) ld; }
```

So when **madc** compiles `mir-gen.c`, the body of `mir_ui2f` is lowered into a
call to `mir.ui2f` — itself. It recurses until the stack dies.

Read off the artifact, not inferred:

```
madc's mir_ui2f:                          gcc's mir_ui2f:
  sub    $0x8,%rsp                          test   %rdi,%rdi
  mov    0x0(%rip),%rax   <- addrpool       js     <fixup>
  call   *%rax                              pxor   %xmm0,%xmm0
  add    $0x8,%rsp                          cvtsi2ss %rdi,%xmm0
  ret                                       ret

  reloc: .mir.addrpool+0x1440  R_X86_64_64  mir.ui2f
         (ui2d 0x1450, ui2ld 0x1460, ld2i 0x1470)
```

gcc emits the conversion INLINE and calls nothing. Located by one-object swap:
only madc's `mir-gen.o` reproduces it; `mir.o`, `mir-debug.o` and
`mir-debug-gdb.o` all match.

This is not a madc codegen bug. It is a **bootstrap cycle in MIR's design**,
invisible until a compiler built on MIR compiled MIR itself.

### 9.3 The fix is Tier 3 — raise MIR — and that is an owner decision

`.claude/rules/lowering-vs-raising.md`: *"Raising MIR is the biggest fork step —
do it only when decided and roadmapped, and design it for upstream."*

- **UI2F / UI2D / UI2LD** expand in **portable MIR IR**, no assembly: test the
  sign; if negative use the odd-bit trick (`(i >>u 1) | (i & 1)`), convert
  signed, double it; else convert signed directly. That is the sequence gcc
  emits, built from `MIR_BGES`/`MIR_URSH`/`MIR_AND`/`MIR_OR`/`MIR_I2F`/
  `MIR_FADD` — all existing insns. Benefits every MIR user (removes a call from
  hot conversion code) and is upstreamable as-is.
- **LD2I** has no portable MIR-IR expansion — long double → int64 needs x87
  (`fisttpll`, or the `fnstcw`/`fldcw` dance). That one is real x86-64 generator
  work with the register allocator in play.

A partial fix is not acceptable here (`finish-plans-fully`): leaving
`mir_ld2i` self-recursive is a landmine that only fires on long-double code.

**Do not hard-code a name test in madc.** Rule #7: the general machinery must
not special-case `mir.ui2f`. The cycle is MIR's to break.

### 9.4 What shipped (`55f2268bf`), and one correction to §9.3

The four helpers, their four `mir.*` alias exports and their `objload_builtin`
entries are **gone** — the names no longer exist on either side of the link.
`get_builtin` keeps only `va_arg` / `va_block_arg`, and a madc-built
`mir-gen.o` now imports exactly one `mir.*` symbol, `mir.arg_memcpy`.

- **UI2F / UI2D** — `expand_uint_to_fp_insn` in machinize. §9.3 called for
  gcc's branchy shape; it is gcc's **arithmetic**, done **branchlessly** —
  `t = i ^ ((i ^ ((i >>u 1) | (i & 1))) & m)` with `m = (int64_t) i >> 63`, then
  `* (fp) ((m & 1) + 1)`. machinize runs *after* the CFG is built, so a new
  basic block there would mean CFG surgery; a select over the sign mask needs
  none, and both forms are correctly rounded.
- **UI2LD** — §9.3 was WRONG to group it with the other two. An x87 long double
  has a 64-bit mantissa, so every `uint64_t` is exactly representable, and the
  sticky-bit trick — correct only where the low bits are being rounded away —
  would lose the low bit of an odd value ≥ 2^63. It splits at 32 bits instead
  (`(ld) hi * 2^32 + (ld) lo`), which is **exact**, not merely rounded.
- **LD2I** — §9.3 said this half needed "real x86-64 generator work with the
  register allocator in play". It did not. It is **one pattern**:
  `fldt m1; fnstcw mu; movzwl mu,r0d; or $0xc00,r0d; mov %r0w,mv; fldcw mv;
  fistpq mt; fldcw mu; mov r0,mt` — gcc's own sequence, using the DESTINATION
  register as its scratch (dead until the last insn, and the source is pushed
  onto the x87 stack first so r0 may safely alias m1's base). No hard-register
  reservation, no RA interaction. Two red-zone scratch slots were added to the
  pattern language, `mu` (-24(%rsp)) and `mv` (-26(%rsp)), beside the existing
  `mt` (-16(%rsp)).

**Evidence.** A 60-line reducer over every rounding boundary is byte-identical
to gcc under the madc JIT, through madc's `.o`, and under a `c2m` whose entire
libmir madc compiled — in `-ei`, `-eg` and `-el`. Differential over **426**
`c-tests` programs against the gcc-built `c2m`: **219 identical, 0
exit-status mismatches**, and `lacc/convert-int-float.c` and
`lacc/convert-unsigned-float.c` — the two real defects — now identical.
`lacc/bitfield-types-init.c` still differs, exactly as §9.1 predicted.

Gate: `tests/testuintfloatconv.mad` (gcc + clang oracles).

### 9.5 What this uncovered: MIR has NO floating → unsigned conversion

`MIR_F2I` / `MIR_D2I` / `MIR_LD2I` are all **signed**, and `cast()` mapped a
`uint64_t` target straight onto them — so `(uint64_t) 1.8e19` answered
9223372036854775808 (the integer indefinite) on every MIR target, silently,
with exit status 0. Pre-existing, not caused by §9.4: the helper that
expansion replaced was `(int64_t) ld`, with exactly this behaviour.

Fixed in c2mir (`8819bb058`), **Tier 2 rather than Tier 3**: adding
`MIR_F2UI`/`D2UI`/`LD2UI` would mean five generators, the interpreter and the
serializers, to buy what a portable six-insn expansion gets exactly —
`c = fp >= 2^63; res = (int64_t) (fp - (fp) c * 2^63) ^ (c << 63)`. Branchless
for the same reason as above: `cast()` emits straight-line code. Every MIR
target gets the fix.

Gate: `tests/testfptouint64.mad` (gcc + clang oracles).
