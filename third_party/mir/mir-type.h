/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* MIR's data types, the one enum every part of MIR shares -- the generator,
   the interpreter and the call helpers generated code calls
   (mir-<target>-calls.c).  Self-contained (no includes, no context): a
   helper source reads it in a build that has no rest of MIR.  */

#ifndef MIR_TYPE_H
#define MIR_TYPE_H

#define TYPE_EL(t) MIR_T_##t

#define MIR_BLK_NUM 5
/* Data types: */
typedef enum {
  /* Integer types of different size: */
  TYPE_EL (I8), TYPE_EL (U8), TYPE_EL (I16), TYPE_EL (U16),
  TYPE_EL (I32), TYPE_EL (U32), TYPE_EL (I64), TYPE_EL (U64),
  TYPE_EL (F), TYPE_EL (D), TYPE_EL (LD),       /* Float or (long) double type */
  TYPE_EL (P), TYPE_EL (BLK),                   /* Pointer, memory blocks */
  TYPE_EL (RBLK) = TYPE_EL (BLK) + MIR_BLK_NUM, /* return block */
  TYPE_EL (V128),                               /* 128-bit vector */
  TYPE_EL (UNDEF), TYPE_EL (BOUND),
} MIR_type_t;

static inline int MIR_int_type_p (MIR_type_t t) {
  return (MIR_T_I8 <= t && t <= MIR_T_U64) || t == MIR_T_P;
}

static inline int MIR_fp_type_p (MIR_type_t t) { return MIR_T_F <= t && t <= MIR_T_LD; }

static inline int MIR_vector_type_p (MIR_type_t t) { return t == MIR_T_V128; }

static inline int MIR_reg_type_p (MIR_type_t t) {
  return t == MIR_T_I64 || MIR_fp_type_p (t) || MIR_vector_type_p (t);
}

static inline int MIR_blk_type_p (MIR_type_t t) { return MIR_T_BLK <= t && t < MIR_T_RBLK; }
static inline int MIR_all_blk_type_p (MIR_type_t t) { return MIR_T_BLK <= t && t <= MIR_T_RBLK; }

#endif /* #ifndef MIR_TYPE_H */
