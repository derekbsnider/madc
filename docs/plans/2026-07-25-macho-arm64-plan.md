# Mach-O / ARM64 AOT — track plan (2026-07-25, owner-directed)

## Goal

`madc -o app --target=arm64-macos` emits a **complete, signed,
runnable Apple Silicon Mach-O executable from the Linux container** —
no macOS build environment, no external toolchain (MIR is the
compiler/assembler/linker, as on ELF). The owner's only macOS device
is a run-only test machine (no dev tools): the e2e test is *copy the
binary over and run it*. Everything before that final gate must be
buildable AND testable on Linux.

**This track is the next `/promote` milestone** (owner 2026-07-25):
master stays at v0.38.0 until Mach-O support lands on develop.

## Owner constraints

- NO external toolchain ever — the emitter produces the finished,
  signed image itself (the ELF-track law, unchanged).
- No macOS build env exists or is planned; the laptop (Apple Silicon)
  runs finished binaries only.
- Target is **arm64** ~~(x86-64 Mach-O is legacy; not in scope)~~ —
  **superseded by owner directive 2026-07-25 (axis B kickoff): BOTH
  targets are in scope** — deliver a *madc-A64_MachO* binary (Apple
  Silicon laptop) AND a *madc-X64_MachO* binary (the owner's Intel Mac
  test device). Owner shorthand maps to the settled triple-style
  naming: A64 = `arm64-macos`, X64 = `x86-64-macos`.
- No slice proliferation: two axes, each shipped complete, each with a
  Linux-side gate.

## Recon facts (2026-07-25, fork @985c2b90, container)

- `mir-gen-aarch64.c` is a full upstream aarch64 code generator and
  **already carries the Darwin ABI arms** (`#if defined(__APPLE__)`:
  all varargs on stack, x18 reserved, etc.) because upstream MIR
  supports Apple Silicon JIT.
- Target selection is **compile-time host detection** in both layers:
  `mir-gen.c:369` (`__x86_64__` → include `mir-gen-x86_64.c`, …) and
  `c2mir.c:36/354` (`x86_64/cx86_64.h` / `aarch64/caarch64.h`).
  Cross-generation from an x86-64 host requires a build-time **target
  override** (a `MIR_GEN_TARGET` / c2mir target macro forcing the
  aarch64 includes + a darwin-ABI flag replacing raw `__APPLE__`
  checks), then an audit of host-arch assumptions in the forced files.
- The object capture layer (`mir-debug.c`) is mostly arch-conditional
  already (32 x86_64-specific mentions; aarch64 DWARF regs present).
  The capture define hooks (incl. S4 bindings) live in the x86-64 gen
  file and need twins in the aarch64 gen.
- The `MIR_object` seam (`MIR_object_emit`, `MIR_object_emit_executable`,
  `MIR_object_read`) is format-agnostic at the API level — the Mach-O
  writer is an alternative assembler behind it.
- Container has the **LLVM-18 Mach-O oracle**: `llvm-otool-18`,
  `llvm-objdump-18`, `llvm-readobj-18`, `llvm-nm-18`. It does NOT yet
  have `qemu-user` or an aarch64 cross gcc — both apt-installable
  (provisioning step, axis A).

## Decomposition — two axes, each with a Linux gate

### Axis A — aarch64 code generation + capture (fork)

Everything except the container format, proven **entirely on Linux**
by reusing the existing ELF writer for aarch64 and running the result
under qemu.

1. Fork cross-target build: `MIR_GEN_TARGET=aarch64` (and the c2mir
   twin) — a second library build (`libmir-aarch64.a`) from the same
   sources; the darwin-ABI arms become a target flag
   (`MIR_TARGET_APPLE`) instead of raw `__APPLE__` so linux-aarch64
   and darwin-arm64 are both selectable from an x86-64 host build.
2. madc cross build against it (`bin/madc` stays host; a
   `--target`-configured build emits AOT artifacts only — JIT
   execution is host-arch by nature, like any cross compiler).
3. aarch64 capture: port the define/reloc hooks (addrpool/GOT model,
   S4 bindings, init_array, RELRO layout) to the aarch64 gen; ELF
   `R_AARCH64_*` relocation set in the writer.
4. **Gate A (all-Linux e2e):** aarch64 ELF executables and `.o` merges
   from the container run correctly under `qemu-aarch64`; gcc parity
   oracle = `aarch64-linux-gnu-gcc` on the same reducers.

### Axis B — Mach-O writer + ad-hoc signature (fork)

Same captured code, different container format.  Two legs (owner
directive 2026-07-25), sequenced X64 → A64: the writer is built once
and proven first against the long-proven x86-64 capture (Intel Macs
run unsigned binaries — format bugs isolate from signing bugs), then
the arm64 leg adds the mandatory ad-hoc signature and rides the
axis-A aarch64 capture.  Deliverables: **madc-X64_MachO** (Intel Mac)
and **madc-A64_MachO** (Apple Silicon laptop) test binaries on the
NAS, each handed over with its exact path + Linux-side evidence.

1. Mach-O64 writer behind the seam: header, `LC_SEGMENT_64`
   (`__TEXT`/`__DATA_CONST`/`__DATA`/`__LINKEDIT`), `LC_SYMTAB`/
   `LC_DYSYMTAB`, `LC_LOAD_DYLIB` (libSystem — macOS mandates dynamic
   libSystem; the addrpool-as-GOT model maps onto dyld bind entries),
   `LC_MAIN`, `LC_UUID`, `LC_BUILD_VERSION` (minos 12.0 default),
   PIE (mandatory on arm64).
2. dyld binding: classic `LC_DYLD_INFO_ONLY` opcodes first (still
   accepted for arm64 on current macOS; chained fixups only if the
   laptop's OS rejects classic — research checkpoint, not a slice).
3. **Ad-hoc code signature** (`LC_CODE_SIGNATURE` + CodeDirectory
   SHA-256 page hashes) generated by the emitter itself — Apple
   Silicon refuses unsigned binaries; ad-hoc signing is pure hashing,
   no certificate involved.
4. `.o` flavor (`MH_OBJECT` + `ARM64_RELOC_*`) so the R4b cache /
   `-r` merge model carries over.
5. **Gate B (Linux):** `llvm-otool`/`llvm-readobj`/`llvm-objdump`
   validate structure, symbols, relocations, and signature layout;
   dyld opcode streams decoded and checked.
6. **Gate B-final (the only macOS steps):** owner copies the binaries
   to the Macs and runs them — the X64 binary on the Intel Mac (can
   land early, pre-signature), the A64 binary on the Apple Silicon
   laptop.  (Owner 2026-07-25: when each Mach-O test binary exists,
   tell the owner exactly where it is — NAS path — along with the
   Linux-side evidence for it.)
   **✅ GREEN 2026-07-25 (owner-run, BOTH Macs):** `madc-A64_MachO`
   and `madc-X64_MachO` both printed
   `madc Mach-O: one three 174 fib(10)=55` and exited 28 — identical
   behavior on Apple Silicon (AMFI accepted the MIR-generated ad-hoc
   signature) and Intel.  The writer is validated on real hardware
   the same day it was written.

### Sequencing

Axis A first — it de-risks everything (codegen, ABI, capture,
relocations) with a fully automated Linux gate before any Mach-O byte
exists. Axis B then rides on proven aarch64 code. The `/promote`
milestone = both gates green + the laptop run.

### Configuration model — SETTLED with the owner (2026-07-25)

Criterion: most sensible AND least refactoring. One target per
compiled stack; no runtime target switching anywhere.

- `libmir.a` = the host build, untouched default (upstream-identical
  behavior; a no-override build must stay behavior-identical).
- `libmir-arm64-macos.a` / `libmir-aarch64-linux.a` = variant builds
  of the same sources with the target defines (opt-in build target /
  configure flag; default builds them not at all).
- `bin/madc` = host binary, unchanged. `bin/madc-arm64-macos` = a
  separate emit-only cross binary linked against the variant lib
  (the gcc cross model — `aarch64-linux-gnu-gcc` ≡ `madc-arm64-macos`);
  run/JIT mode errors loudly ("cross build is emit-only"). No `--target`
  runtime dispatch, no dlopen, no symbol prefixing; a thin exec-dispatch
  driver can be sugar later without changing any of this.
- madc's own front-end target dependencies (type layout — notably
  `long double`: 16 bytes x86-64-linux, fp128 aarch64-linux, plain
  double arm64-macos) are compile-time selected in the cross build by
  the same defines.
- Selector shape (owner design): separate primitive knobs
  (`MIR_TARGET_AARCH64` arch, `MIR_TARGET_APPLE` OS) + pair helpers
  (`MIR_TARGET_ARM64_MACOS` = both) + a validation block rejecting
  unsupported combinations — `mir-target.h`, additive.
- Upstream-file footprint (least-refactoring choice): ONE include line
  in `mir.h`, SIX small `#if`-chain edits (mir-gen.c, mir.c, c2mir.c
  ×3 + multiarch header dirs, mir-debug.c constants), and TWO
  commented include-wraps (`#define __APPLE__ 1` around
  `#include "mir[-gen]-aarch64.c"` for the arm64-macos variant on a
  non-Apple host — all 22 `__APPLE__` sites in those files were
  audited target-ABI, none host-side; the wrap keeps both aarch64
  files pristine for upstream merges). Everything else is additive.

### Decided defaults (change only on owner directive)

- ~~arm64 only~~ **BOTH arm64 AND x86-64 Mach-O (owner directive
  2026-07-25, axis B kickoff)**; still no arm64e (pointer
  authentication) — plain arm64 slice runs fine on Apple Silicon.
  - The `x86_64-macos` de-risking step is ADOPTED and sequenced FIRST:
    validate the Mach-O writer/dyld/signature with the already-proven
    x86-64 capture before the arm64 leg (Intel Macs also run unsigned
    binaries, so it isolates format bugs from signing).
  - x86-64-Darwin ABI audit result (2026-07-25 recon): MIR's x86-64
    gen/ABI code (`mir-gen-x86_64.c`, `cx86_64-ABI-code.c`) has ZERO
    `__APPLE__` sites — Darwin x86-64 is SysV at MIR's level.  The only
    Apple arms are in the `mirc_x86_64_*` predefined-macro/typedef
    headers + the `c2mir.c` include chains, i.e. the same
    target-selection treatment axis A gave aarch64.  `long double` on
    x86-64-Darwin = 80-bit x87 / 16 bytes = identical to linux — the
    front end needs zero layout changes for the X64 leg.
- Classic dyld info before chained fixups; minos 12.0.
- Container provisioning (`qemu-user`, `gcc-aarch64-linux-gnu`) —
  DONE 2026-07-25 (loop proven: cross-compiled exit-42 binary runs
  under qemu-aarch64).

### Known cross-fidelity audit items (found in recon, not yet handled)

- `c2mir/aarch64/caarch64.h:49` `typedef long double mir_ldouble;` —
  c2mir folds target long-double constants in the HOST's type: wrong
  precision for aarch64-linux (fp128) and wrong size for arm64-macos
  (= double). Needs a target-selected typedef (3-line conditional) and
  a fidelity decision for the fp128 case.
- Execution paths in a cross build (code pages, thunks, ffi, interp)
  compile against target files but must never run; the first cross
  build attempt generates the concrete stub/guard worklist.

## Axis A step 2 — aarch64 capture + ELF relocs (LANDED 2026-07-25)

The whole gen+writer step shipped as one piece on the fork
(`feature/cross-target-claude`); all four Linux functional legs are green
from the x86-64 container:

1. `c2m -fobject` (aarch64-target build) emits an aarch64 ELF `.o`;
   `aarch64-linux-gnu-gcc` links it (the SYSTEM linker independently
   validates our relocations) and it runs under qemu-aarch64 with
   output + exit code identical to the native-gcc reference.
2. `MIR_object_emit_executable` ET_EXEC runs under qemu (our own linker,
   aarch64 `_start` stub — no external toolchain).
3. Same for PIE (`ET_DYN` + `DF_1_PIE` + RELATIVE pool slots).
4. Two-TU `.o` merge (`MIR_object_read` ×2 → emit PIE) resolves a
   cross-TU call + global and runs correctly (exit 42).

Design (mirrors the x86-64 capture, PIC addrpool-as-GOT):

- **Gen** (`mir-gen-aarch64.c`, all object-mode-only — the JIT path is
  byte-identical): constraint `'j'` (REF && object mode; `Z`/`N` movz/movk
  constraints now REJECT refs in object mode, so a leak fails the pattern
  match loudly), pattern `{MIR_MOV, "r j", adrp+ldr}` ahead of the movz/movk
  rows, replacement char `'p'` records a `const_ref` (pc = adrp insn; ldr at
  pc+4 by convention); machinize already rewrites every call target through
  `MOV temp, ref; blr`, so ONE pool pattern covers data refs and calls.
  Switch tables: the `T` adr is emitted as a relocated adrp+add pair in
  object mode, slots move to the pool (ABS64 vs the text section symbol),
  dead in-blob bytes zeroed.  `target_object_capture` mirrors x86-64
  (8-byte pool entries — no SSE alignment need; S4 weak/linkonce binding
  args identical).
- **Reloc kinds** (`mir-debug.h`): `MIR_OBJ_RELOC_AARCH64_ADR_PG_HI21` /
  `_LDST64_LO12` / `_ADD_LO12`; like PC32 they are bias-invariant and never
  dynamic — only pool ABS64 slots go dynamic (RELATIVE in PIC images,
  dyn-ABS64 for imports), exactly the x86-64 model.
- **Writer** (`mir-debug.c`): central per-target helpers (`obj_kind_rtype`,
  `obj_rtype_kind`, `obj_apply_field_reloc` — ADRP immhi:immlo / LDST64
  imm12>>3 / ADD imm12 bitfield patchers) replace the inline x86 constants
  at every seam: `.o` emit, exe emit (validate + apply), in-process load
  apply, merge read-back; `OBJ_TARGET_SUPPORTED_P` gates create/emit/read
  (x86-64 + aarch64).  aarch64 `_start` stub (16 insns, page-pair
  bias-invariant, patched with the same field patchers), interp default
  `/lib/ld-linux-aarch64.so.1`, `OBJX_PAGE` 64K for aarch64 (gcc/binutils
  max-page-size canon — a 4K-aligned binary fails on 64K-page kernels),
  `e_machine`/`elf_assemble` take `MIR_DEBUG_EM`.  `emit_frame` was
  host-gated (`__x86_64__`) — now target-gated with an aarch64 CIE arm
  (RA=x30, code align 4, CIE-default FDE rows; prologue templates are the
  debug track's rung, not this slice).
- ⚠️ Trap found in testing: FOUR EM gates existed, not one —
  `MIR_object_create`, `MIR_object_emit` (line ~1384), `elf_assemble`'s
  hardcoded `EM_X86_64` (~1644), and the reader's scan gate (~2501).  The
  first compile+emit attempt failed on the second one.

Remaining axis A after this: cross-madc build wiring (variant lib +
emit-only `bin/madc-arm64-macos` / test aarch64-linux binary + front-end
long-double sizing) → GATE A = madc-emitted aarch64 exe/.o-merge under
qemu; the `mir_ldouble` fidelity item still open (`caarch64.h:49`).

## Axis A step 1 recon — the override-site map (2026-07-25, container provisioned)

Container now has `qemu-user` + `gcc-aarch64-linux-gnu` +
`libc6-dev-arm64-cross`; the loop is proven (cross-compiled exit-42
binary runs under `qemu-aarch64`).

Host-detection sites the target override must switch (fork):

| Site | What it selects | Cross treatment |
|------|-----------------|-----------------|
| `mir-gen.c:369` | gen backend include (`mir-gen-<arch>.c`) | target-select — the capture lives here |
| `mir.c:7349` | runtime/thunk layer (`mir-<arch>.c`) | target-select; audit: execution-only parts (code pages, icache flush, ffi thunks) compile out or stub in a cross build |
| `c2mir/c2mir.c:36,354,15577` | target ABI headers + code (`<arch>/c<arch>*.{h,c}`) | target-select (aarch64 dirs exist upstream) |
| `mir-debug.c:149` | DWARF FP reg + ELF `EM_*` | target-select (aarch64 arm already present) |
| `mir-debug.c:863` | `.debug_frame` prologue byte patterns | needs an aarch64 arm (x86-64-only today) |
| `mir-debug.c:2238` | gdb-JIT self-registration | HOST feature — stays host-gated |
| `mir.c:4698,5053` | small host conditionals | audit individually |
| `mir-debug.c` reloc emission | `R_X86_64_*` constants | `R_AARCH64_*` twins — the bulk of capture porting (addrpool/GOT model on aarch64 ADRP/LDR pairs) |

Scaffold shape: a `mir-target.h` decision block — `MIR_TARGET_AARCH64`
/ `MIR_TARGET_X86_64` (+ `MIR_TARGET_APPLE` for the `__APPLE__` arms in
`mir-gen-aarch64.c`) defaulting to host detection when no override is
given — and the sites above switch on `MIR_TARGET_IS_*`. Safety gate
for the refactor: a no-override build must behave identically (full
suites green). Then `make BUILD_DIR=build-aarch64` with the override
compiles the cross variant; whatever breaks is the audit worklist.

## Axis B design (2026-07-25, recon complete — implementation basis)

**Oracle loop proven on Linux (container):** `clang-18 -target
{x86_64,arm64}-apple-macos12 -c` emits reference Mach-O objects;
`ld64.lld-18` (provisioned 2026-07-25, oracle-only like axis A's
qemu/cross-gcc) links them against a hand-written minimal
`libSystem.tbd` stub into **reference MH_EXECUTE binaries for both
arches** — including lld's linker-signed ad-hoc CodeDirectory on
arm64, a byte-level signature reference.  `llvm-otool-18 -l` of the
arm64 reference pins the canonical shape: __PAGEZERO(0..0x100000000) /
__TEXT @0x100000000 (16K pages) / __DATA_CONST(__got) / __DATA /
__LINKEDIT; LC order: segments, DYLD_INFO_ONLY, SYMTAB, DYSYMTAB,
LOAD_DYLINKER(/usr/lib/dyld), UUID, BUILD_VERSION(macos 12.0), MAIN,
LOAD_DYLIB(libSystem.B), FUNCTION_STARTS, DATA_IN_CODE,
CODE_SIGNATURE.

**Writer placement:** new fork file `mir-macho.c`, `#include`d by
`mir-debug.c` when `MIR_TARGET_APPLE_P` (house style — included
target `.c` files).  `MIR_object_emit_executable` diverts to
`macho_emit_executable` for Apple targets; `shared_p` errors loudly
(no dylib emission — deliberately out of scope, there is no
libmadc.dylib by design).  Everything upstream of the assembler
(capture, sections, symbols, relocs) is shared unchanged.

**Section mapping (ELF → Mach-O):** .text → `__TEXT,__text` (after
header+LCs); .mir.addrpool → `__DATA_CONST,__mir_addrpool`
(S_REGULAR — the addrpool-as-GOT model needs NO stubs, NO lazy
binding, NO indirect symbol table; dyld reads the rebase/bind
streams); .init_array → `__DATA_CONST,__mod_init_func`
(S_MOD_INIT_FUNC_POINTERS — dyld runs entries after fixups); .data →
`__DATA,__data`; .bss → `__DATA,__bss` (S_ZEROFILL vmsize tail).
Page size 16K arm64 / 4K x86-64; link base 0x100000000; PIE always
(MH_PIE; dyld slides are page-multiple so the aarch64 page-pair
field relocs stay bias-invariant, exactly like ELF).

**Relocation mapping:** field kinds (PC32, aarch64 page pairs)
resolve at emit with the same `obj_apply_field_reloc` patchers.
Internal ABS64 → link vaddr BAKED into the file bytes + a
REBASE_TYPE_POINTER opcode (dyld ADDS the slide to the stored value
— the Mach-O twin of RELATIVE, but the file carries the addend
result).  Import ABS64 → zero slot + BIND opcode (two-level, library
ordinal = libSystem, `_`-prefixed symbol name,
BIND_OPCODE_SET_ADDEND_SLEB when nonzero).  **LC_MAIN kills the
`_start` stub entirely** — entryoff = the entry symbol's file offset;
dyld's libdyld glue passes argc/argv/envp/apple and exits with main's
return.  No __libc_start_main slot, no stub patching.

**Symbols:** nlist_64, local → extdef → undef ordering with
LC_DYSYMTAB ranges (no indirect table, nindirectsyms=0); `_` prefix
prepended at write time; undefs carry SET_LIBRARY_ORDINAL(libSystem).
Export trie empty (executables export nothing — ELF import-only
dynsym parity).  LC_FUNCTION_STARTS emitted for real (uleb deltas
from defined func syms); LC_DATA_IN_CODE empty (lld parity);
LC_UUID = deterministic truncated SHA-256 of image content
(reproducible builds, no entropy source).

**Ad-hoc signature (arm64 mandatory; emitted for both arches):**
`LC_CODE_SIGNATURE` → SuperBlob{CodeDirectory v0x20400, flags
CS_ADHOC|CS_LINKER_SIGNED, SHA-256, 4K signing pages (independent of
VM page size), nSpecialSlots=0, execSeg = __TEXT with
CS_EXECSEG_MAIN_BINARY, codeLimit = signature dataoff (16-aligned)}.
All blob integers big-endian.  SHA-256 implemented inline in the
fork (no external deps — the no-toolchain law).  Identifier: new
`MIR_object_exec_params` field (madc passes the output basename;
NULL → "mir.out").

**mirc predefines:** the `mirc_{x86_64,aarch64}_*.h` Apple/linux arms
switch on HOST macros today — cross builds flip them to
`MIR_TARGET_APPLE_P` / `!MIR_TARGET_APPLE_P` directly (precedent:
`caarch64.h:49`; these headers are small and upstream-stable, so
direct edits beat include-wraps here).  The arm64-macos predefine arm
already exists upstream (`__APPLE__ 1`, `__arm64__`,
`__darwin_va_list`, LDBL==DBL); x86_64-macos gets its twin.
x86_64+APPLE knob: `MIR_TARGET_X86_64_MACOS` pair helper +
validation relax in mir-target.h (recon: ZERO `__APPLE__` sites in
x86-64 gen/ABI code — Darwin x86-64 is SysV at MIR's level; no
include-wraps needed for the X64 leg).

**Deliberately out of scope (unchanged owner defaults):** dylib
emission, chained fixups (classic LC_DYLD_INFO_ONLY; research
checkpoint only if the laptop OS refuses), arm64e, __cstring/
__unwind_info niceties.  macOS SDK headers remain a later owner
decision — fork-level gate reducers declare their own prototypes;
the host /usr/include leak into an Apple-target compile is a known
audit item for the madc MODE step.

**Axis B sequencing:** (1) x86_64-macos target knob + mirc arms →
(2) MH_EXECUTE writer, X64 leg, Gate B structural validation vs the
lld reference → X64 test binary to NAS (runs unsigned on the Intel
Mac — can ship to the owner pre-signature) → (3) ad-hoc signature →
(4) A64 leg (16K pages, BUILD_VERSION mandatory) + signature → A64
test binary to NAS → (5) MH_OBJECT .o flavor (+ ld64.lld as the
independent reloc validator, the axis-A system-linker trick) →
(6) madc cross MODEs (`cross-x86-64-macos`, `cross-arm64-macos`) →
madc-emitted deliverables **madc-X64_MachO** / **madc-A64_MachO**.

## Axis B steps 1–3 LANDED (2026-07-25) — writer + both cross madcs

Fork `feature/macho-writer-claude` (@e760b5fb target selection,
@77c1e056 writer), madc `feature/aot-macho-claude` (@bb42ddbd cross
MODEs).  All Linux-side gates green the same day the axis started:

1. **Target selection**: `MIR_TARGET_X86_64_MACOS` pair; the
   `mirc_{x86_64,aarch64}_*` predefine/typedef headers switch on
   `MIR_TARGET_APPLE_P` instead of host macros (wchar signedness, LDBL,
   int64_t spelling, darwin va_list, OS predefine block); two
   target-code-semantics sites in mir.c/c2mir.c (`__darwin*` handling)
   flip too.  Default build behavior-identical (probe-verified); both
   macOS variant c2ms pass target-fact probes.
2. **The writer** (`mir-macho.c`, ~870 lines, #included by mir-debug.c
   under `MIR_TARGET_APPLE_P`): MH_EXECUTE per the design section
   above, including the inline SHA-256 and the linker-signed ad-hoc
   CodeDirectory.  Gate B evidence: otool layout matches the
   clang+ld64.lld reference **including identical segment addresses on
   arm64 (16K pages)**; rebase/bind streams decode (all pool + data
   pointers rebased, `_printf` → libSystem at its pool slot); adrp
   pairs land on the pool page; disassembly valid on both arches;
   two-TU merge → signed PIE; python re-hash independently verifies
   every signature page and file-ends-at-signature; ELF object +
   load-object suites 0 failures (default build untouched).
3. **madc cross MODEs**: `make -C src cross-x86-64-macos` /
   `cross-arm64-macos` → emit-only `bin/madc-x86-64-macos` /
   `bin/madc-arm64-macos`.  Cross modes now use per-mode ../lib
   archive names (a cross build could previously clobber the
   host-facing libmadc.a — latent since axis A).  `MADC_CROSS_APPLE`:
   base-lib sonames become cover-analysis-only (implicit libSystem);
   runtime-needing programs fail AT EMIT with a clear message;
   signature identifier = output basename.  Lanes proven: pure C
   `-o`, `.mad` `-o` (madc's include machinery covers `<stdio.h>` —
   the raw-c2m SDK gap does not bite the .mad lane), `-c` ×2 →
   madc-link, run-lane refusal.
4. **Deliverables built and on the NAS**: `bin/madc-X64_MachO` +
   `bin/madc-A64_MachO` (from `tmp`-side `macho_hello.c`: switch→pool
   table, global data refs, internal calls, recursion, printf import;
   expected `madc Mach-O: one three 174 fib(10)=55`, exit 28).

Remaining in axis B: **MH_OBJECT `.o` flavor** (+ ld64.lld as the
independent reloc validator) — the `-c` artifacts are ELF-container
dev vehicles until then (madc's own read-back/link consumes them
fine, as proven above); **front-end long double** stayed untouched by
design (madc models `long double` as the double-spelling; c2mir owns
target layout via the axis-A `mir_ldouble` fix — LD-in-struct layout
is a target-independent audit item, not a Mach-O blocker);
**Gate B-final** = the owner's Macs.

## The larger goal — madc-release ON macOS (owner, 2026-07-25 post-Gate-B-final)

After running both test binaries, the owner clarified the destination:
**a madc-release binary that runs natively on macOS** — madc-the-
compiler on the Macs, not only madc targeting them.  This track
("madc targets macOS") is one step along that way; the remaining
distance is a **porting/build-provisioning track, route not yet
decided** (owner decision pending):

- **Route 1 — osxcross-style cross BUILD tool on Linux:** clang +
  cctools + a macOS SDK build madc's C++ codebase for the Mac from
  the container.  Does not violate the no-external-toolchain law
  (that law governs madc's *product path* — what madc emits — not
  how madc itself is built; madc is already built with g++/clang).
  Needs the Apple SDK on Linux (owner owns Macs/Xcode; extraction is
  the owner's licensing call) + a macOS port pass over madc's source
  (dlopen sonames, /proc/self/exe, glibc-isms) + MIR JIT on macOS
  (upstream supports Apple Silicon JIT; the fork carries it).
- **Route 2 — build on a Mac:** Xcode CLT on one of the owner's Macs
  (contradicts the laptop-is-run-only stance; the Intel Mac could be
  the build host if the owner re-decides).  Same madc source-port
  pass; simplest toolchain story, moves build/test off the container.
- **Route 3 — self-hosting:** madc emits its own compiler as Mach-O.
  The true north star, but gated on the self-hosting gap list
  (std::function, containers, streams breadth) — far out; not the
  vehicle for this goal.

Either serious route also needs the target-side runtime story (a
Mach-O libmadc / runtime for non-runtime-free programs) and real SDK
headers for full-fidelity target compiles — both already flagged as
owner decisions in this plan.

## Risks

- Host-arch assumption audit in cross-included gen/c2mir files (sizes,
  endianness fine; `__APPLE__`/host intrinsics leakage is the thing to
  hunt).
- Darwin dyld details (bind opcode subtleties, signature alignment
  rules) are documented but fiddly; the LLVM tools + a reference
  clang-produced binary from godbolt/CI can serve as byte-level
  oracles without a Mac.
- qemu-aarch64 fidelity is high for user-mode integer/FP code; it is a
  codegen gate, not a performance gate.
