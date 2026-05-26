/* This file is a part of Gecko project.
   Copyright (C) 2026 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef BITMAP_H

#define BITMAP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include "vlobject.h"

#if !defined(BITMAP_ENABLE_CHECKING) && !defined(NDEBUG)
#define BITMAP_ENABLE_CHECKING
#endif

#ifndef BITMAP_ENABLE_CHECKING
#define BITMAP_ASSERT(EXPR, OP) ((void) (EXPR))

#else
static inline void gp_bitmap_assert_fail (const char *op) {
  fprintf (stderr, "wrong %s for a bitmap", op);
  assert (0);
}

#define BITMAP_ASSERT(EXPR, OP) (void) ((EXPR) ? 0 : (gp_bitmap_assert_fail (#OP), 0))

#endif

#define BITMAP_WORD_BITS 64

typedef uint64_t bitmap_el_t;

#define BITMAP_WORD_BYTES sizeof (bitmap_el_t)

typedef vlo_t bitmap_t;
typedef const vlo_t *const_bitmap_t;

static inline void bitmap_create2 (bitmap_t *res, gp_allocator_t *alloc, size_t init_bits_num) {
  VLO_CREATE (*res, alloc, (init_bits_num + BITMAP_WORD_BITS - 1) / BITMAP_WORD_BITS * BITMAP_WORD_BYTES);
}

static inline void bitmap_create (bitmap_t *res, gp_allocator_t *alloc) { bitmap_create2 (res, alloc, 0); }

static inline void bitmap_destroy (bitmap_t *bm) { VLO_DELETE (*bm); }

static inline void bitmap_clear (bitmap_t *bm) { VLO_NULLIFY (*bm); }

static inline void bitmap_expand (bitmap_t *bm, size_t nb) {
  size_t len = VLO_LENGTH (*bm);
  size_t new_len = (nb + BITMAP_WORD_BITS - 1) / BITMAP_WORD_BITS * BITMAP_WORD_BYTES;

  if (len >= new_len) return;
  VLO_EXPAND (*bm, new_len - len);
  memset ((char *) VLO_BEGIN (*bm) + len, 0, new_len - len);
}

static __attribute__ ((noinline)) bool bitmap_bit_p (const bitmap_t *bm, size_t nb) {
  size_t nw, sh, len = VLO_LENGTH (*bm) / BITMAP_WORD_BYTES;
  bitmap_el_t *addr = VLO_BEGIN (*bm);

  if (nb >= BITMAP_WORD_BITS * len) return 0;
  nw = nb / BITMAP_WORD_BITS;
  sh = nb % BITMAP_WORD_BITS;
  return (addr[nw] >> sh) & 1;
}

static inline bool bitmap_set_bit_p (bitmap_t *bm, size_t nb) {
  size_t nw, sh;
  bitmap_el_t *addr;
  int res;

  bitmap_expand (bm, nb + 1);
  addr = VLO_BEGIN (*bm);
  nw = nb / BITMAP_WORD_BITS;
  sh = nb % BITMAP_WORD_BITS;
  res = ((addr[nw] >> sh) & 1) == 0;
  addr[nw] |= (bitmap_el_t) 1 << sh;
  return res;
}

static inline bool bitmap_clear_bit_p (bitmap_t *bm, size_t nb) {
  size_t nw, sh, len = VLO_LENGTH (*bm) / BITMAP_WORD_BYTES;
  bitmap_el_t *addr = VLO_BEGIN (*bm);
  int res;

  if (nb >= BITMAP_WORD_BITS * len) return 0;
  nw = nb / BITMAP_WORD_BITS;
  sh = nb % BITMAP_WORD_BITS;
  res = (addr[nw] >> sh) & 1;
  addr[nw] &= ~((bitmap_el_t) 1 << sh);
  return res;
}

static inline bool bitmap_set_or_clear_bit_range_p (bitmap_t *bm, size_t nb, size_t len, bool set_p) {
  size_t nw, lsh, rsh, range_len;
  bitmap_el_t mask, *addr;
  int res = 0;

  bitmap_expand (bm, nb + len);
  addr = VLO_BEGIN (*bm);
  while (len > 0) {
    nw = nb / BITMAP_WORD_BITS;
    lsh = nb % BITMAP_WORD_BITS;
    rsh = len >= BITMAP_WORD_BITS - lsh ? 0 : BITMAP_WORD_BITS - (nb + len) % BITMAP_WORD_BITS;
    mask = ((~(bitmap_el_t) 0) >> (rsh + lsh)) << lsh;
    if (set_p) {
      res |= (~addr[nw] & mask) != 0;
      addr[nw] |= mask;
    } else {
      res |= (addr[nw] & mask) != 0;
      addr[nw] &= ~mask;
    }
    range_len = BITMAP_WORD_BITS - rsh - lsh;
    len -= range_len;
    nb += range_len;
  }
  return res;
}

static inline bool bitmap_set_bit_range_p (bitmap_t *bm, size_t nb, size_t len) {
  return bitmap_set_or_clear_bit_range_p (bm, nb, len, true);
}

static inline bool bitmap_clear_bit_range_p (bitmap_t *bm, size_t nb, size_t len) {
  return bitmap_set_or_clear_bit_range_p (bm, nb, len, false);
}

static inline void bitmap_copy (bitmap_t *dst, const bitmap_t *src) {
  size_t dst_len = VLO_LENGTH (*dst) / BITMAP_WORD_BYTES;
  size_t src_len = VLO_LENGTH (*src) / BITMAP_WORD_BYTES;

  if (dst_len >= src_len)
    VLO_SHORTEN (*dst, (dst_len - src_len) * BITMAP_WORD_BYTES);
  else
    bitmap_expand (dst, src_len * BITMAP_WORD_BITS);
  memcpy (VLO_BEGIN (*dst), VLO_BEGIN (*src), src_len * sizeof (bitmap_el_t));
}

static inline bool bitmap_equal_p (const bitmap_t *bm1, const bitmap_t *bm2) {
  const bitmap_t *temp_bm;
  size_t i, temp_len, bm1_len = VLO_LENGTH (*bm1) / BITMAP_WORD_BYTES;
  size_t bm2_len = VLO_LENGTH (*bm2) / BITMAP_WORD_BYTES;
  bitmap_el_t *addr1, *addr2;

  if (bm1_len > bm2_len) {
    temp_bm = bm1;
    bm1 = bm2;
    bm2 = temp_bm;
    temp_len = bm1_len;
    bm1_len = bm2_len;
    bm2_len = temp_len;
  }
  addr1 = VLO_BEGIN (*bm1);
  addr2 = VLO_BEGIN (*bm2);
  if (memcmp (addr1, addr2, bm1_len * sizeof (bitmap_el_t)) != 0) return false;
  for (i = bm1_len; i < bm2_len; i++)
    if (addr2[i] != 0) return false;
  return true;
}

static inline bool bitmap_intersect_p (const bitmap_t *bm1, const bitmap_t *bm2) {
  size_t i, min_len, bm1_len = VLO_LENGTH (*bm1) / BITMAP_WORD_BYTES;
  size_t bm2_len = VLO_LENGTH (*bm2) / BITMAP_WORD_BYTES;
  bitmap_el_t *addr1 = VLO_BEGIN (*bm1);
  bitmap_el_t *addr2 = VLO_BEGIN (*bm2);

  min_len = bm1_len <= bm2_len ? bm1_len : bm2_len;
  for (i = 0; i < min_len; i++)
    if ((addr1[i] & addr2[i]) != 0) return true;
  return false;
}

static inline bool bitmap_empty_p (const bitmap_t *bm) {
  size_t i, len = VLO_LENGTH (*bm) / BITMAP_WORD_BYTES;
  bitmap_el_t *addr = VLO_BEGIN (*bm);

  for (i = 0; i < len; i++)
    if (addr[i] != 0) return false;
  return true;
}

static inline bitmap_el_t bitmap_el_max2 (bitmap_el_t el1, bitmap_el_t el2) { return el1 < el2 ? el2 : el1; }

static inline bitmap_el_t bitmap_el_max3 (bitmap_el_t el1, bitmap_el_t el2, bitmap_el_t el3) {
  if (el1 <= el2) return el2 < el3 ? el3 : el2;
  return el1 < el3 ? el3 : el1;
}

/* Return the number of bits set in BM.  */
static inline size_t bitmap_bit_count (const bitmap_t *bm) {
  size_t i, len = VLO_LENGTH (*bm) / BITMAP_WORD_BYTES;
  bitmap_el_t el, *addr = VLO_BEGIN (*bm);
  size_t count = 0;

  for (i = 0; i < len; i++) {
    if ((el = addr[i]) != 0) {
      for (; el != 0; el >>= 1)
        if (el & 1) count++;
    }
  }
  return count;
}

static inline bool bitmap_op2 (bitmap_t *dst, const bitmap_t *src1, const bitmap_t *src2,
                               bitmap_el_t (*op) (bitmap_el_t, bitmap_el_t)) {
  size_t i, len, bound, src1_len, src2_len;
  bitmap_el_t old, *dst_addr, *src1_addr, *src2_addr;
  bool change_p = false;

  src1_len = VLO_LENGTH (*src1) / BITMAP_WORD_BYTES;
  src2_len = VLO_LENGTH (*src2) / BITMAP_WORD_BYTES;
  len = bitmap_el_max2 (src1_len, src2_len);
  bitmap_expand (dst, len * BITMAP_WORD_BITS);
  dst_addr = VLO_BEGIN (*dst);
  src1_addr = VLO_BEGIN (*src1);
  src2_addr = VLO_BEGIN (*src2);
  for (bound = i = 0; i < len; i++) {
    old = dst_addr[i];
    if ((dst_addr[i] = op (i >= src1_len ? 0 : src1_addr[i], i >= src2_len ? 0 : src2_addr[i])) != 0)
      bound = i + 1;
    if (old != dst_addr[i]) change_p = true;
  }
  VLO_SHORTEN (*dst, (len - bound) * BITMAP_WORD_BYTES);
  return change_p;
}

static inline bitmap_el_t bitmap_el_and (bitmap_el_t el1, bitmap_el_t el2) { return el1 & el2; }

static inline bool bitmap_and (bitmap_t *dst, bitmap_t *src1, bitmap_t *src2) {
  return bitmap_op2 (dst, src1, src2, bitmap_el_and);
}

static inline bitmap_el_t bitmap_el_and_compl (bitmap_el_t el1, bitmap_el_t el2) { return el1 & ~el2; }

static inline bool bitmap_and_compl (bitmap_t *dst, bitmap_t *src1, bitmap_t *src2) {
  return bitmap_op2 (dst, src1, src2, bitmap_el_and_compl);
}

static inline bitmap_el_t bitmap_el_ior (bitmap_el_t el1, bitmap_el_t el2) { return el1 | el2; }

static inline bool bitmap_ior (bitmap_t *dst, bitmap_t *src1, bitmap_t *src2) {
  return bitmap_op2 (dst, src1, src2, bitmap_el_ior);
}

static inline bool bitmap_op3 (bitmap_t *dst, const bitmap_t *src1, const bitmap_t *src2,
                               const bitmap_t *src3,
                               bitmap_el_t (*op) (bitmap_el_t, bitmap_el_t, bitmap_el_t)) {
  size_t i, len, bound, src1_len, src2_len, src3_len;
  bitmap_el_t old, *dst_addr, *src1_addr, *src2_addr, *src3_addr;
  bool change_p = false;

  src1_len = VLO_LENGTH (*src1) / BITMAP_WORD_BYTES;
  src2_len = VLO_LENGTH (*src2) / BITMAP_WORD_BYTES;
  src3_len = VLO_LENGTH (*src3) / BITMAP_WORD_BYTES;
  len = bitmap_el_max3 (src1_len, src2_len, src3_len);
  bitmap_expand (dst, len * BITMAP_WORD_BITS);
  dst_addr = VLO_BEGIN (*dst);
  src1_addr = VLO_BEGIN (*src1);
  src2_addr = VLO_BEGIN (*src2);
  src3_addr = VLO_BEGIN (*src3);
  for (bound = i = 0; i < len; i++) {
    old = dst_addr[i];
    if ((dst_addr[i] = op (i >= src1_len ? 0 : src1_addr[i], i >= src2_len ? 0 : src2_addr[i],
                           i >= src3_len ? 0 : src3_addr[i]))
        != 0)
      bound = i + 1;
    if (old != dst_addr[i]) change_p = true;
  }
  VLO_SHORTEN (*dst, (len - bound) * BITMAP_WORD_BYTES);
  return change_p;
}

static inline bitmap_el_t bitmap_el_ior_and (bitmap_el_t el1, bitmap_el_t el2, bitmap_el_t el3) {
  return el1 | (el2 & el3);
}

/* DST = SRC1 | (SRC2 & SRC3).  Return true if DST changed.  */
static inline bool bitmap_ior_and (bitmap_t *dst, bitmap_t *src1, bitmap_t *src2, bitmap_t *src3) {
  return bitmap_op3 (dst, src1, src2, src3, bitmap_el_ior_and);
}

static inline bitmap_el_t bitmap_el_ior_and_compl (bitmap_el_t el1, bitmap_el_t el2, bitmap_el_t el3) {
  return el1 | (el2 & ~el3);
}

/* DST = SRC1 | (SRC2 & ~SRC3).  Return true if DST changed.  */
static inline bool bitmap_ior_and_compl (bitmap_t *dst, bitmap_t *src1, bitmap_t *src2, bitmap_t *src3) {
  return bitmap_op3 (dst, src1, src2, src3, bitmap_el_ior_and_compl);
}

typedef struct {
  bitmap_t *bitmap;
  size_t nbit;
} bitmap_iterator_t;

static inline void bitmap_iterator_init (bitmap_iterator_t *iter, bitmap_t *bitmap) {
  iter->bitmap = bitmap;
  iter->nbit = 0;
}

static inline bool bitmap_iterator_next (bitmap_iterator_t *iter, size_t *nbit) {
  const size_t el_bits_num = sizeof (bitmap_el_t) * CHAR_BIT;
  size_t curr_nel = iter->nbit / el_bits_num, len = VLO_LENGTH (*iter->bitmap) / BITMAP_WORD_BYTES;
  bitmap_el_t el, *addr = VLO_BEGIN (*iter->bitmap);

  for (; curr_nel < len; curr_nel++, iter->nbit = curr_nel * el_bits_num)
    if ((el = addr[curr_nel]) != 0)
      for (el >>= iter->nbit % el_bits_num; el != 0; el >>= 1, iter->nbit++)
        if (el & 1) {
          *nbit = iter->nbit++;
          return true;
        }
  return false;
}

#define FOREACH_BITMAP_BIT(iter, bitmap, nbit) \
  for (bitmap_iterator_init (&(iter), (bitmap)); bitmap_iterator_next (&(iter), &(nbit));)

#endif /* #ifndef BITMAP_H */
