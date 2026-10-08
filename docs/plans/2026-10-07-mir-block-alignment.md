# MIR block arguments carry their alignment — vnmakarov/mir#482 part 2

Owner (2026-10-07): do it the gcc way, in our fork first; open a PR — or, if
the result is too divergent to send as code, contribute the design as a
comment on #482.

## The defect (measured)

A 16-byte-aligned aggregate passed by value lands at the next 8-byte stack
offset; gcc (SysV x86-64 3.2.3, AAPCS64 C.8/C.12-C.14) puts it at a 16-byte
aligned one, and on aarch64 starts it at an even GPR. A gcc-compiled callee
taking `struct { long double }` after seven `long`s: gcc+gcc `2.5`; upstream
c2m, our c2m and `bin/madc` callers `-nan`, exit 0 (container `tmp/blk482/`).
A SILENT wrong answer for every madc program passing such a struct to C.

`MIR_T_BLK`..`+4` encode the passing CLASS (x86-64 uses all five); the size
rides in `MIR_var_t.size` (protos) and in the call operand's `disp`. Nothing
carries alignment, and no block placement anywhere rounds (the one exception
is riscv64's `MIR_T_BLK + 1` "aligned register pair", consumed but produced by
nothing in our c2mir).

## Design — the gcc model

gcc: alignment is a property of the argument's TYPE; each target's
`function_arg_boundary` hook places it. MIR equivalent: the block carries its
alignment; each target's placement reads it through ONE predicate per target.

- **IR.** `MIR_var_t` gains `align` (bytes; 0 = the default 8, so every
  existing positional initializer stays correct). The call operand for a block
  carries the same value — required: a vararg block has no proto var. Carrier
  on `MIR_mem_t`: a small field beside `volatile_p` (log2 or bytes; settle on
  reading the bitfield layout), set only for block call operands; it is not
  part of operand identity beyond what `disp` (size) already is — check
  `MIR_op_eq_p` / `MIR_op_hash_step`.
- **va_block_arg.** Its operands are (res, va, size, ncase); the alignment
  must reach the builtin. Prefer packing it into the existing `ncase` operand
  (class in the low bits, log2 alignment above) over a fifth operand: five
  target lowerings, the descriptor, the interpreter and mir2c all read four.
- **Formats.** Text: `blk1:16:a16(x)`-style suffix only when align > 8 (old
  text reads unchanged). Binary: an optional prefix tag, the `TAG_VOLATILE`
  precedent — no version bump needed for old files.
- **Checks.** `MIR_new_insn_arr` already compares a block operand's size with
  the proto var's (mir.c ~2763): compare alignment there too.
- **Caches/dedup keyed on size** must add alignment: c2mir `proto_hash` /
  `proto_eq` (c2mir.c ~20849/20867), the interpreter's ff stub cache
  (`ff_interface_hash` / `_eq`, mir-interp.c ~2347), `_MIR_arg_desc_t`
  (mir.h ~873) which the ff_call shims read.

## Sites (inventory 2026-10-07, read-only recon; line numbers approximate)

| Area | Where | Change |
|---|---|---|
| c2mir x86-64 | `target_add_arg_proto`, `target_add_call_arg_op` (cx86_64-ABI-code.c) | set align = `type_align` when > 8 |
| c2mir generic / aarch64 | `simple_add_arg_proto`, `simple_add_call_arg_op` (c2mir.c ~16323/16342); caarch64 delegates (its :19-26 already records the even-NGRN gap) | same |
| c2mir va_arg | `gen` N_CALL va_arg → `MIR_VA_BLOCK_ARG` (c2mir.c ~19859) | pack align into ncase |
| x86-64 caller | `machinize_call` stack block: small (~497) and memcpy (~571) | round `arg_stack_size` to 16 first — the `stack_arg_16_p` shape (4fbffc77e) |
| x86-64 callee | `target_machinize` stack block (~1131) | round `mem_size` first |
| x86-64 shim | `_MIR_get_ff_call` block stack copy (~581) | round `sp_offset` (precedent :528/:534) |
| x86-64 va | `va_block_arg_builtin` overflow path (mir-x86_64.c ~77-168) | round `overflow_arg_area` (precedent `va_arg_builtin` :53-68) |
| win64 | by-reference copies at `block_offset` (machinize_call ~554-566) | copy 16-aligned for a 16-aligned type (MS x64) |
| aarch64 caller | `machinize_call` pass 1 (~308) and pass 2 regs (~355) / stack (~374) / by-ref copy (~396) — in lockstep | C.8 even NGRN; 16-aligned stack slot; aligned copy |
| aarch64 callee | `target_machinize` small block: register save area (~875, 8-granular today) and stack (~894) | even NGRN, aligned save slot and stack slot |
| aarch64 shims | `_MIR_get_ff_call` both passes (~399, ~433, ~445); Apple interp shim loops (~597, ~616-660) | same rule |
| aarch64 va | `va_block_arg_builtin` linux + Apple (mir-aarch64.c ~108-146) | round gr_offs to even / __stack and arg_area to 16 |
| owner | mir-aarch64.h beside `stack_arg_slot_start` / `stack_arg_16_p` | ONE block-alignment predicate per target, read by every site above |
| interpreter | `call()` arg descs (~2437/2443), ff cache, `interp()` incoming block (~2679) | carry align |
| inliner | `add_blk_move` allocas `(size+7)/8*8` unaligned (mir.c ~4475) | align the copy |
| ppc64 / s390x / riscv64 | listed only | keep behaviour; riscv64's BLK+1 may become derived from align later |

## Oracle

Extend the interop gate (`scripts/vector_abi_gate.sh`, `tests/abi/libvecnative.c`
+ `vec_ffcall.c`; host compiler on one side, c2m `-eg`/`-ei` and madc on the
other; x86-64 host, aarch64 qemu, the Mac) with aligned-block probes in both
directions: `struct { _Alignas (16) long a; long b; }` and `struct { __int128 v; }`
(register class; regs exhausted → stack), `struct { long double x; }` (x86-64
MEMORY class; an HFA on aarch64 — #477's domain, so x86-64 only), a 32-byte
aligned memory-class struct, each declared after an odd number of eightbytes
and through `...`, and callbacks (gcc caller → c2m callee). `_Alignas` on a
member parses as of `314125d7e`.

## Slices

1. IR + formats + checks + c2mir producers (no placement change yet; the gate's
   new probes red as measured).
2. x86-64 SysV: caller, callee, ff_call, va_block_arg — probes green on the host.
3. aarch64 linux + Apple: the same set, one predicate in mir-aarch64.h — qemu + Mac.
4. win64 aligned by-reference copies; interpreter, inliner.
5. Upstream: a clean branch on a8ab7c31 if the diff ports; else a design comment on #482.

Thread-safety contract: per-context IR data, immutable after creation — no new
shared state.
