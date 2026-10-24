/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* The aarch64 helpers generated code calls (mir-gen-aarch64.c): va_arg and a
   block va_arg (the "mir.va_arg" / "mir.va_block_arg" imports) and the word
   copy of a by-value block argument over 128 bytes ("mir.blk_mov").
   Context-free -- memcpy, the type numbers and the argument classes
   (mir-aarch64-abi.h), nothing of a MIR context -- so one text serves two
   builds: libmir's (mir-aarch64.c includes it; the JIT calls these, and libmir
   exports them for AOT objects under the import names), and the copy an
   executable or object carries itself, which madc compiles from this text
   (MADC_RT_IMAGE: src/madc_crt.cpp) as gcc's driver links libgcc's helpers
   into an image.  There the functions are file-local and the image defines
   only the import names.  */

#include <stddef.h>
#include <stdint.h>
#include "mir-type.h"
#include "mir-aarch64-abi.h"

#ifdef MADC_RT_IMAGE
#define MIR_CALL_HELPER static
void *memcpy (void *, const void *, size_t);
#else
#define MIR_CALL_HELPER
#endif

#if defined(__APPLE__)
struct aarch64_va_list {
  uint64_t *arg_area;
};
#else
struct aarch64_va_list {
  /* address following the last (highest addressed) named incoming
     argument on the stack, rounded upwards to a multiple of 8 bytes,
     or if there are no named arguments on the stack, then the value
     of the stack pointer when the function was entered. */
  void *__stack;
  /* the address of the byte immediately following the general
     register argument save area, the end of the save area being
     aligned to a 16 byte boundary. */
  void *__gr_top;
  /* the address of the byte immediately following the FP/SIMD
     register argument save area, the end of the save area being
     aligned to a 16 byte boundary. */
  void *__vr_top;
  int __gr_offs; /* set to 0 – ((8 – named_gr) * 8) */
  int __vr_offs; /* set to 0 – ((8 – named_vr) * 16) */
};
#endif

MIR_CALL_HELPER void *va_arg_builtin (void *p, uint64_t t) {
  struct aarch64_va_list *va = p;
  MIR_type_t type = t;
#if defined(__APPLE__)
  void *a;

  if (stack_arg_16_p (type)) {
    /* a 16-byte argument takes a 16-byte-aligned pair of slots (AAPCS64 C.14) */
    va->arg_area = (uint64_t *) (((uint64_t) va->arg_area + 15) / 16 * 16);
    a = va->arg_area;
    va->arg_area += 2;
  } else {
    a = va->arg_area;
    va->arg_area++;
  }
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  if (type == MIR_T_F || type == MIR_T_I32) a = (char *) a + 4; /* 2nd word of doubleword */
#endif
#else
  /* the SIMD/FP class: F, D, LD and the 128-bit vector (a Q register: the vr
     save area holds every FP register at a 16-byte stride) */
  int fp_p = fp_class_type_p (type);
  int size16_p = stack_arg_16_p (type);
  void *a;

  if (fp_p && va->__vr_offs < 0) {
    a = (char *) va->__vr_top + va->__vr_offs;
    va->__vr_offs += 16;
  } else if (!fp_p && va->__gr_offs < 0) {
    a = (char *) va->__gr_top + va->__gr_offs;
    va->__gr_offs += 8;
  } else {
    if (size16_p) va->__stack = (void *) (((uint64_t) va->__stack + 15) / 16 * 16);
    a = va->__stack;
    va->__stack = (char *) va->__stack + (size16_p ? 16 : 8);
  }
#endif
  return a;
}

MIR_CALL_HELPER void va_block_arg_builtin (void *res, void *p, size_t s, uint64_t ncase) {
  struct aarch64_va_list *va = p;
  MIR_type_t type = (MIR_type_t) (MIR_T_BLK + ncase);
  size_t hfa_n = target_hfa_members (type, s);
#if defined(__APPLE__)
  void *a = (void *) va->arg_area;
  if (hfa_n != 0) { /* HFAs are passed by value on the stack */
    va->arg_area += target_hfa_stack_size (type, s) / sizeof (uint64_t);
    if (res != NULL) memcpy (res, a, s);
    return;
  }
  if (s <= 2 * 8) {
    va->arg_area += (s + sizeof (uint64_t) - 1) / sizeof (uint64_t);
  } else {
    a = *(void **) a;
    va->arg_area++;
  }
  if (res != NULL) memcpy (res, a, s);
#else
  void *a;
  long size = (s + 7) / 8 * 8;

  if (hfa_n != 0) {
    /* An HFA is in the FP reg save area (one 16-byte slot per member) if all its
       members fit, otherwise on the stack in memory layout; __vr_offs advances
       either way, as in GCC.  */
    size_t el_size = target_hfa_el_size (type);
    int offs = va->__vr_offs;

    if (offs < 0) {
      va->__vr_offs = offs + 16 * (int) hfa_n;
      if (va->__vr_offs <= 0) {
        if (res != NULL)
          for (size_t n = 0; n < hfa_n; n++)
            memcpy ((char *) res + n * el_size, (char *) va->__vr_top + offs + 16 * n, el_size);
        return;
      }
    }
    va->__stack = (void *) stack_arg_slot_start ((uint64_t) va->__stack,
                                                 target_hfa_stack_align (type));
    a = va->__stack;
    va->__stack = (char *) va->__stack + target_hfa_stack_size (type, s);
    if (res != NULL) memcpy (res, a, s);
    return;
  }
  if (size <= 2 * 8 && va->__gr_offs + size > 0) { /* not enough regs to pass: */
    a = va->__stack;
    va->__stack = (char *) va->__stack + size;
    va->__gr_offs += size;
    if (res != NULL) memcpy (res, a, s);
    return;
  }
  if (size > 2 * 8) size = 8;
  if (va->__gr_offs < 0) {
    a = (char *) va->__gr_top + va->__gr_offs;
    va->__gr_offs += size;
  } else {
    a = va->__stack;
    va->__stack = (char *) va->__stack + size;
  }
  if (s > 2 * 8) a = *(void **) a; /* address */
  if (res != NULL) memcpy (res, a, s);
#endif
}

/* The word copy of a by-value block argument over 128 bytes (mir-gen-aarch64.c
   gen_blk_mov); smaller ones are copied inline. */
MIR_CALL_HELPER void mir_blk_mov (uint64_t *to, uint64_t *from, uint64_t nwords) {
  for (; nwords > 0; nwords--) *to++ = *from++;
}
