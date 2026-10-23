/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* The x86-64 helpers generated code calls (mir-gen-x86_64.c): va_arg and a
   block va_arg (the "mir.va_arg" / "mir.va_block_arg" imports) and the copy
   of a by-value block argument ("mir.arg_memcpy").  Context-free -- memcpy
   and the type numbers, nothing of a MIR context -- so one text serves two
   builds: libmir's (mir-x86_64.c includes it; the JIT calls these, and libmir
   exports them for AOT objects under the import names), and the copy an
   executable or object carries itself, which madc compiles from this text
   (MADC_RT_IMAGE: src/madc_crt.cpp) as gcc's driver links libgcc's helpers
   into an image.  There the functions are file-local and the image defines
   only the import names.  */

#include <stddef.h>
#include <stdint.h>
#include "mir-type.h"

#ifdef MADC_RT_IMAGE
#define MIR_CALL_HELPER static
void *memcpy (void *, const void *, size_t);
#else
#define MIR_CALL_HELPER
#endif

#ifndef _WIN32
struct x86_64_va_list {
  uint32_t gp_offset, fp_offset;
  uint64_t *overflow_arg_area, *reg_save_area;
};

MIR_CALL_HELPER void *va_arg_builtin (void *p, uint64_t t) {
  struct x86_64_va_list *va = p;
  MIR_type_t type = t;
  /* the SSE class: float, double and the 128-bit vector (__m128) -- each xmm
     register is saved in a 16-byte slot of the register save area */
  int fp_p = type == MIR_T_F || type == MIR_T_D || MIR_vector_type_p (type);
  int size16_p = type == MIR_T_LD || MIR_vector_type_p (type);
  void *a;

  if (fp_p && va->fp_offset <= 160) {
    a = (char *) va->reg_save_area + va->fp_offset;
    va->fp_offset += 16;
  } else if (!fp_p && type != MIR_T_LD && va->gp_offset <= 40) {
    a = (char *) va->reg_save_area + va->gp_offset;
    va->gp_offset += 8;
  } else {
    /* a 16-byte argument takes a 16-byte ALIGNED slot of the overflow area
       (SysV 3.5.7 step 7: the alignment of the type, 16 for long double and
       __m128) */
    if (size16_p)
      va->overflow_arg_area = (uint64_t *) (((uint64_t) va->overflow_arg_area + 15) / 16 * 16);
    a = va->overflow_arg_area;
    va->overflow_arg_area += size16_p ? 2 : 1;
  }
  return a;
}

MIR_CALL_HELPER void va_block_arg_builtin (void *res, void *p, size_t s, uint64_t ncase) {
  struct x86_64_va_list *va = p;
  size_t size = ((s + 7) / 8) * 8;
  void *a = va->overflow_arg_area;
  union {
    uint64_t i;
    double d;
  } u[2];

  switch (ncase) {
  case 1:
    if (va->gp_offset + size > 48) break;
    u[0].i = *(uint64_t *) ((char *) va->reg_save_area + va->gp_offset);
    va->gp_offset += 8;
    if (size > 8) {
      u[1].i = *(uint64_t *) ((char *) va->reg_save_area + va->gp_offset);
      va->gp_offset += 8;
    }
    if (res != NULL) memcpy (res, &u, s);
    return;
  case 2:
    /* Register-exhaustion demotion (SysV): the caller passes the block in
       XMM registers only when EVERY eightbyte fits (mir-gen-x86_64.c
       machinize: fp_arg_num and fp_arg_num+1 both available), otherwise the
       WHOLE block goes to the overflow area and the fp counter is left for
       later smaller args.  The fp save area is 128 bytes at offsets
       48..176, one 16-byte slot per XMM, so a one-eightbyte block needs
       fp_offset <= 160 and a two-eightbyte block fp_offset <= 144.  This
       case had NO check (cases 1/3/4 have theirs), so the 9th SSE eightbyte
       of a vararg call read past the save area (c-testsuite 00204: the
       fifth float-pair struct printed garbage).  */
    if (va->fp_offset + (size > 8 ? 32 : 16) > 176) break;
    u[0].d = *(double *) ((char *) va->reg_save_area + va->fp_offset);
    va->fp_offset += 16;
    if (size > 8) {
      u[1].d = *(double *) ((char *) va->reg_save_area + va->fp_offset);
      va->fp_offset += 16;
    }
    if (res != NULL) memcpy (res, &u, s);
    return;
  case 3:
  case 4:
    if (va->fp_offset > 160 || va->gp_offset > 40) break;
    if (ncase == 3) {
      u[0].i = *(uint64_t *) ((char *) va->reg_save_area + va->gp_offset);
      u[1].d = *(double *) ((char *) va->reg_save_area + va->fp_offset);
    } else {
      u[0].d = *(double *) ((char *) va->reg_save_area + va->fp_offset);
      u[1].i = *(uint64_t *) ((char *) va->reg_save_area + va->gp_offset);
    }
    /* An SSE eightbyte occupies a 16-byte slot in the register save area
       (each XMM is stored in 16 bytes), so fp_offset must advance by 16 --
       matching the SSE-only path (case 2) and va_arg_builtin.  Advancing by
       only 8 made the next mixed-class struct's SSE field read an
       overlapping/stale slot.  The INTEGER eightbyte is 8 bytes. */
    va->fp_offset += 16;
    va->gp_offset += 8;
    if (res != NULL) memcpy (res, &u, s);
    return;
  default: break;
  }
  if (res != NULL) memcpy (res, a, s);
  va->overflow_arg_area += size / 8;
}

#else

struct x86_64_va_list {
  uint64_t *arg_area;
};

MIR_CALL_HELPER void *va_arg_builtin (void *p, uint64_t t) {
  struct x86_64_va_list *va = p;
  void *a = va->arg_area;
  (void) t;
  va->arg_area++;
  return a;
}

MIR_CALL_HELPER void va_block_arg_builtin (void *res, void *p, size_t s, uint64_t ncase) {
  struct x86_64_va_list *va = p;
  void *a = s <= 8 ? va->arg_area : *(void **) va->arg_area; /* pass by pointer */
  (void) ncase;
  if (res != NULL) memcpy (res, a, s);
  va->arg_area++;
}

#endif

/* The copy of a by-value block argument the stack-passing convention makes
   (mir-gen-x86_64.c): memcpy's contract.  The JIT binds memcpy itself. */
MIR_CALL_HELPER void *mir_arg_memcpy (void *dest, const void *src, size_t n) {
  return memcpy (dest, src, n);
}
