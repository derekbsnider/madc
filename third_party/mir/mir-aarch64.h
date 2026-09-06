/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   A common include file for mir-aarch64.c and mir-gen-aarch64.c
*/

#include "mir.h"

#define TARGET_NOP 0xd503201f

#define HREG_EL(h) h##_HARD_REG
#define REP_SEP ,
enum {
  REP8 (HREG_EL, R0, R1, R2, R3, R4, R5, R6, R7),
  REP8 (HREG_EL, R8, R9, R10, R11, R12, R13, R14, R15),
  REP8 (HREG_EL, R16, R17, R18, R19, R20, R21, R22, R23),
  REP8 (HREG_EL, R24, R25, R26, R27, R28, R29, R30, SP),
  ZR_HARD_REG = SP_HARD_REG,
  REP8 (HREG_EL, V0, V1, V2, V3, V4, V5, V6, V7),
  REP8 (HREG_EL, V8, V9, V10, V11, V12, V13, V14, V15),
  REP8 (HREG_EL, V16, V17, V18, V19, V20, V21, V22, V23),
  REP8 (HREG_EL, V24, V25, V26, V27, V28, V29, V30, V31),
};
#undef REP_SEP

static const char *const target_hard_reg_names[] = {
  "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10", "r11", "r12",
  "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25",
  "r26", "r27", "r28", "r29", "r30", "sp",  "v0",  "v1",  "v2",  "v3",  "v4",  "v5",  "v6",
  "v7",  "v8",  "v9",  "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19",
  "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
};

#define MAX_HARD_REG V31_HARD_REG

/* Hard regs not used in machinized code, preferably call used ones. */
static const MIR_reg_t TEMP_INT_HARD_REG1 = R9_HARD_REG, TEMP_INT_HARD_REG2 = R10_HARD_REG;
static const MIR_reg_t TEMP_FLOAT_HARD_REG1 = V16_HARD_REG, TEMP_FLOAT_HARD_REG2 = V17_HARD_REG;
static const MIR_reg_t TEMP_DOUBLE_HARD_REG1 = V16_HARD_REG, TEMP_DOUBLE_HARD_REG2 = V17_HARD_REG;
static const MIR_reg_t TEMP_LDOUBLE_HARD_REG1 = V16_HARD_REG;
static const MIR_reg_t TEMP_LDOUBLE_HARD_REG2 = V17_HARD_REG;

static inline int target_hard_reg_type_ok_p (MIR_reg_t hard_reg, MIR_type_t type) {
  assert (hard_reg <= MAX_HARD_REG);
  /* The 128-bit vector is an FP-class value: it lives in the SIMD/FP file as a
     Q register -- the shape the 128-bit long double already has there. */
  return MIR_fp_type_p (type) || MIR_vector_type_p (type) ? hard_reg >= V0_HARD_REG
                                                          : hard_reg < V0_HARD_REG;
}

static inline int target_fixed_hard_reg_p (MIR_reg_t hard_reg) {
  assert (hard_reg <= MAX_HARD_REG);
#if defined(__APPLE__)
  if (hard_reg == R18_HARD_REG) return TRUE;
#endif
  return (hard_reg == R29_HARD_REG /*FP*/ || hard_reg == SP_HARD_REG
          || hard_reg == TEMP_INT_HARD_REG1 || hard_reg == TEMP_INT_HARD_REG2
          || hard_reg == TEMP_FLOAT_HARD_REG1 || hard_reg == TEMP_FLOAT_HARD_REG2
          || hard_reg == TEMP_DOUBLE_HARD_REG1 || hard_reg == TEMP_DOUBLE_HARD_REG2
          || hard_reg == TEMP_LDOUBLE_HARD_REG1 || hard_reg == TEMP_LDOUBLE_HARD_REG2);
}

static int target_locs_num (MIR_reg_t loc, MIR_type_t type) {
  return loc > MAX_HARD_REG && (type == MIR_T_LD || MIR_vector_type_p (type)) ? 2 : 1;
}

/* The SIMD/FP argument class: F, D, LD and the 128-bit vector -- AAPCS64 C.1
   allocates a Short Vector exactly like a floating-point value (v[NSRN]).
   Read by the machinizer (mir-gen-aarch64.c) and the call shims
   (mir-aarch64.c): one owner for both directions of every call. */
static inline int fp_class_type_p (MIR_type_t type) {
  return type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD || MIR_vector_type_p (type);
}

/* A 16-byte, 16-byte-aligned stack argument slot -- and a Q register in the
   register file: the 128-bit long double and every vector.  (With MIR_LD_IS_D
   the long double is 8 bytes and MIR_T_LD is canonicalized away before it
   reaches here; the predicate is written per type so Apple's 8-byte long
   double never shares the vector's slot.) */
static inline int stack_arg_16_p (MIR_type_t type) {
  return (type == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16) || MIR_vector_type_p (type);
}

/* The stack slot of a scalar argument: its byte size, which is also its
   alignment.  AAPCS64 C.12-C.14 give every stack argument at least 8 bytes
   (16 for a 16-byte type); Apple's arm64 ABI ("Writing ARM64 Code for Apple
   Platforms": function arguments may consume stack slots that are not
   multiples of 8 bytes) packs a NON-variadic argument at its natural size and
   alignment -- a char takes one byte, a short two, an int or a float four --
   while a variadic one keeps the 8-byte slot Apple's va_arg advances by.  A
   block (composite) keeps its 8-byte alignment on both.  One owner for the
   machinizer's call site and callee (mir-gen-aarch64.c) and both call shims
   (mir-aarch64.c): a MIR caller and a MIR callee agree with each other under
   any rule, only the platform compiler on the other side can tell. */
static inline size_t stack_arg_slot_size (MIR_type_t type, int vararg_p) {
  if (stack_arg_16_p (type)) return 16;
#if defined(__APPLE__)
  if (!vararg_p) {
    if (type == MIR_T_I8 || type == MIR_T_U8) return 1;
    if (type == MIR_T_I16 || type == MIR_T_U16) return 2;
    if (type == MIR_T_I32 || type == MIR_T_U32 || type == MIR_T_F) return 4;
  }
#else
  (void) vararg_p;
#endif
  return 8;
}

/* where a slot of the given size starts: the running offset rounded up to the
   slot's alignment, which is its size */
static inline size_t stack_arg_slot_start (size_t offset, size_t slot_size) {
  return (offset + slot_size - 1) / slot_size * slot_size;
}

/* The memory type of the slot's load or store: the argument's own type for the
   SIMD/FP class and for a slot narrower than 8 bytes (a packed Apple char /
   short / int, read and written at that width, a load extending by the
   type), an 8-byte integer otherwise. */
static inline MIR_type_t stack_arg_mem_type (MIR_type_t type, int vararg_p) {
  if (fp_class_type_p (type) || stack_arg_slot_size (type, vararg_p) < 8) return type;
  return MIR_T_I64;
}
