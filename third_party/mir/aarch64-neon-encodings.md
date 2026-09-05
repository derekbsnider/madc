# aarch64 Advanced SIMD encodings used by mir-gen-aarch64.c (MIR_T_V128)

Every base word in the vector rows of the aarch64 pattern table was taken from
this probe, not from memory: the mnemonics were assembled with
`aarch64-linux-gnu-as` (binutils, Debian cross toolchain) and read back with
`aarch64-linux-gnu-objdump -d` on 2026-09-05. Each mnemonic appears twice —
all-`v0`/`x0` operands give the base word, `v1,v2,v3` / `x1,x2` confirm the
field positions: Rd bits [4:0], Rn [9:5], Rm [20:16], the same `vd`/`vn`/`vm`
fields the FP patterns use. Q-form arrangements: `size` = bits [23:22]
(00 `.16b`, 01 `.8h`, 10 `.4s`, 11 `.2d`); float `sz` = bit 22 (0 `.4s`, 1 `.2d`).

Re-derive with:

```
aarch64-linux-gnu-as -o neon_probe.o neon_probe.s
aarch64-linux-gnu-objdump -d neon_probe.o
```

| word | mnemonic | MIR insn |
|---|---|---|
| 4ea01c00 | mov v0.16b, v0.16b (= orr Vd,Vn,Vn) | VMOV r r (the LDMOV word) |
| 4ea31c41 | orr v1.16b, v2.16b, v3.16b | VOR; field check |
| 4e201c00 | and v0.16b, v0.16b, v0.16b | VAND |
| 6e201c00 | eor v0.16b, v0.16b, v0.16b | VXOR |
| 6e205800 | mvn (not) v0.16b, v0.16b | VNEF second insn |
| 4e208400 / 4e608400 / 4ea08400 / 4ee08400 | add .16b / .8h / .4s / .2d | VADDI8/16/32/64 |
| 6e208400 / 6e608400 / 6ea08400 / 6ee08400 | sub .16b / .8h / .4s / .2d | VSUBI8/16/32/64 |
| 4e609c00 / 4ea09c00 | mul .8h / .4s | VMULI16/32 (NEON has no .2d mul; MIR has no VMULI64) |
| 4e20d400 / 4e60d400 | fadd .4s / .2d | VADDF32/64 |
| 4ea0d400 / 4ee0d400 | fsub .4s / .2d | VSUBF32/64 |
| 6e20dc00 / 6e60dc00 | fmul .4s / .2d | VMULF32/64 |
| 6e20fc00 / 6e60fc00 | fdiv .4s / .2d | VDIVF32/64 |
| 6e208c00 / 6e608c00 / 6ea08c00 / 6ee08c00 | cmeq .16b / .8h / .4s / .2d | VEQI8/16/32/64 |
| 4e203400 / 4e603400 / 4ea03400 / 4ee03400 | cmgt (signed) .16b / .8h / .4s / .2d | VGTI8/16/32/64 |
| 4e20e400 / 4e60e400 | fcmeq .4s / .2d | VEQF32/64, VNEF first insn |
| 6ea0e400 / 6ee0e400 | fcmgt .4s / .2d | VLTF32/64 with operands swapped (a < b == b > a) |
| 6e20e400 / 6e60e400 | fcmge .4s / .2d | VLEF32/64 with operands swapped |
| 6e204400 / 6e604400 / 6ea04400 / 6ee04400 | ushl .16b / .8h / .4s / .2d | logical shifts (a negative count shifts right) |
| 4e204400 / 4e604400 / 4ea04400 / 4ee04400 | sshl .16b / .8h / .4s / .2d | arithmetic right shifts (negative count) |
| 6e20b800 / 6e60b800 / 6ea0b800 / 6ee0b800 | neg .16b / .8h / .4s / .2d | count negation for the lane-count right shifts |
| cb0003e0 | neg x0, x0 (Rd [4:0], Rm [20:16]) | count negation for the scalar-count right shifts |
| 4e020c00 / 4e040c00 / 4e080c00 | dup v0.8h, w0 / v0.4s, w0 / v0.2d, x0 | broadcast of the scalar count |
| 4e083c00 | umov (mov) x0, v0.d[0] | the scalar count = low 64 bits of the count vector |
| 3ce06800 / 3ca06800 | ldr / str q0, [x0, x0] (option LSL, S = 1: index scaled by 16) | VMOV r mv / mv r (pattern word 3ce00800 / 3ca00800, the `m` replacement fills the option bits) |
| 3dc00000 / 3d800000 | ldr / str q0, [x0] (imm12 scaled by 16: #16 -> 3dc00400) | VMOV r Mv / Mv r |

Probe source (`tmp/mirq/neon_probe.s` in the madc tree at the time; reproduced
here so the table can be regenerated):

```
	orr v0.16b, v0.16b, v0.16b      orr v1.16b, v2.16b, v3.16b
	and v0.16b, v0.16b, v0.16b      eor v0.16b, v0.16b, v0.16b
	not v0.16b, v0.16b
	add v0.{16b,8h,4s,2d}, v0, v0    sub v0.{16b,8h,4s,2d}, v0, v0
	mul v0.{8h,4s}, v0, v0
	fadd/fsub/fmul/fdiv v0.{4s,2d}, v0, v0
	cmeq/cmgt v0.{16b,8h,4s,2d}, v0, v0
	fcmeq/fcmgt/fcmge v0.{4s,2d}, v0, v0
	ushl/sshl v0.{16b,8h,4s,2d}, v0, v0
	neg v0.{16b,8h,4s,2d}, v0       neg x0, x0
	dup v0.8h, w0   dup v0.4s, w0   dup v0.2d, x0
	umov x0, v0.d[0]
	ldr/str q0, [x0, x0]   ldr/str q0, [x0, #16]   ldr/str q0, [x0]
```
