/* This file is a part of Gecko (GLR parser) project.
   Copyright (C) 2026 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* Simple high-quality multiplicative hash passing rurban-smhasher, the same speed as VMUM on keys
   upto 16 bytes and 10% slower on keys upto 32 bytes. For targets without 128-bit arithmetic, the
   hash function is 25% slower.  Hash for the same key can be different on LE and BE architectures.
   To get endian-independent hash, use hash_strict which makes hash a bit slower on BE
   machines.
 */
#ifndef __HASH__
#define __HASH__

#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(__i386__) || defined(__PPC64__) || defined(__s390__) || defined(__m32c__) \
  || defined(cris) || defined(__CR16__) || defined(__vax__) || defined(__m68k__) || defined(__aarch64__)     \
  || defined(_M_AMD64) || defined(_M_IX86)
#define HASH_UNALIGNED_ACCESS 1
#else
#define HASH_UNALIGNED_ACCESS 0
#endif

#if (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)) || defined(_MSC_VER)
#define LENDIAN 1
#else
#define LENDIAN 0
#endif

static inline uint64_t get_key_part (const uint8_t *v, size_t len, int relax_p) {
  if (relax_p || LENDIAN) {
#if HASH_UNALIGNED_ACCESS
    if (len == 8) return *(uint64_t *) v;
    if (len == 7) return *(uint32_t *) v | (uint64_t) *(uint16_t *) (v + 4) << 32 | (uint64_t) v[6] << 48;
    if (len == 6) return *(uint32_t *) v | (uint64_t) *(uint16_t *) (v + 4) << 32;
    if (len == 5) return *(uint32_t *) v | (uint64_t) v[4] << 32;
    if (len == 4) return *(uint32_t *) v;
    if (len == 3) return *(uint16_t *) v | (uint64_t) v[2] << 16;
    if (len == 2) return *(uint16_t *) v;
    return v[0];
#endif
  }
  uint64_t tail = 0;
  for (ptrdiff_t i = (ptrdiff_t) len - 1; i >= 0; i--) tail = tail << 8 | (uint64_t) v[i];
  return tail;
}

static inline uint64_t mum (uint64_t v, uint64_t c) {
#if defined(__SIZEOF_INT128__)
  __uint128_t r = (__uint128_t) v * (__uint128_t) c;
  return (uint64_t) (r >> 64) + (uint64_t) r;
#endif
  uint64_t v1 = v >> 32, v2 = (uint32_t) v, c1 = c >> 32, c2 = (uint32_t) c;
  uint64_t rm = v2 * c1 + v1 * c2;
  return v1 * c1 + v2 * c2 + (rm >> 32) + (rm << 32);
}

static inline uint64_t final (uint64_t v) { return mum (v, v); }

static const uint64_t ps[] = {
  0x50c07b9351011316ull, 0xce26a1f4e66e3d1full, 0x396d79c90c86b1edull, 0x3739b72e11fc4684ull,
  0x7fe4adc8400af08eull, 0xfa2e47475b11a3ceull, 0xd519635f1d7b9242ull, 0x866d04bd866130fcull,
  0x6132e913fd0ae2c9ull, 0xaeacc8d99a02880dull, 0xf196fbab2ef62dbbull, 0x62a6a4ae6d3f5fddull,
  0x3147dbedb6618cf4ull, 0x18dc2a4e6e46b7f5ull, 0x98ce62d3e346f804ull, 0x3e507b6626b9c9c6ull,
};
static inline uint64_t hash_1 (const void *key, size_t len, uint64_t seed, int relax_p) {
  const uint8_t *v = (const uint8_t *) key;
  uint64_t r = mum (seed + len, ps[0]);

  for (; len >= 16; len -= 16, v += 16) {
    r ^= mum (get_key_part (v, 8, relax_p) + ps[1], get_key_part (v + 8, 8, relax_p) + ps[2]);
    r ^= mum (r, ps[3]);
  }
#define CONSUME(s, o, l, n) (s ^ mum (get_key_part (v + o, l, relax_p) + ps[n], ps[n + 1]))
  switch (len) {
  case 15: return final (CONSUME (CONSUME (r, 0, 8, 0), 8, 7, 14));
  case 14: return final (CONSUME (CONSUME (r, 0, 8, 0), 8, 6, 12));
  case 13: return final (CONSUME (CONSUME (r, 0, 8, 0), 8, 5, 10));
  case 12: return final (CONSUME (CONSUME (r, 0, 8, 0), 8, 4, 8));
  case 11: return final (CONSUME (CONSUME (r, 0, 8, 0), 8, 3, 6));
  case 10: return final (CONSUME (CONSUME (r, 0, 8, 0), 8, 2, 4));
  case 9: return final (CONSUME (CONSUME (r, 0, 8, 0), 8, 1, 2));
  case 8: return final (CONSUME (r, 0, 8, 0));
  case 7: return final (CONSUME (r, 0, 7, 14));
  case 6: return final (CONSUME (r, 0, 6, 12));
  case 5: return final (CONSUME (r, 0, 5, 10));
  case 4: return final (CONSUME (r, 0, 4, 8));
  case 3: return final (CONSUME (r, 0, 3, 6));
  case 2: return final (CONSUME (r, 0, 2, 4));
  case 1: return final (CONSUME (r, 0, 1, 2));
  case 0: return final (r);
  }
  return r;
}

static inline uint64_t hash (const void *key, size_t len, uint64_t seed) {
  return hash_1 (key, len, seed, 1);
}

static inline uint64_t hash_strict (const void *key, size_t len, uint64_t seed) {
  return hash_1 (key, len, seed, 0);
}

static inline uint64_t hash_init (uint64_t seed) { return seed + 1; }
static inline uint64_t hash_step (uint64_t h, uint64_t key) {
  return mum (h, ps[0]) ^ mum (key ^ ps[1], ps[2]);
}
static inline uint64_t hash_finish (uint64_t h) { return final (h); }

static inline uint64_t hash64 (uint64_t key, uint64_t seed) {
  return hash_finish (hash_step (hash_init (seed), key));
}

#endif
