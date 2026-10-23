/* A struct over 128 bytes passed by value: MIR's aarch64 generator copies it
   to the caller's argument block through its helper "mir.blk_mov", which
   libmir kept static and nothing exported, so an executable needing it could
   not link.  The image now carries MIR's copy (madc's crt:mir_calls startup
   member).  Two such arguments, and one after eight integer registers, so
   the copies sit beside loaded argument registers. */
int printf (const char *, ...);

typedef struct { long v[20]; } big; /* 160 bytes: copied by mir.blk_mov */

__attribute__((noinline)) static long weigh (big b) {
  long t = 0;
  for (int i = 0; i < 20; i++) t += b.v[i] * (i + 1);
  return t;
}

__attribute__((noinline)) static long pair (long a, big b, big c) {
  return a + weigh (b) - weigh (c);
}

__attribute__((noinline)) static long late (long a, long b, long c, long d, long e, long f,
                                           long g, long h, big x) {
  return a + b + c + d + e + f + g + h + weigh (x);
}

int main (void) {
  big b, c;
  for (int i = 0; i < 20; i++) {
    b.v[i] = 3 * i + 1;
    c.v[i] = 20 - i;
  }
  printf ("weigh %ld\n", weigh (b));
  printf ("pair %ld\n", pair (7, b, c));
  printf ("late %ld\n", late (1, 2, 3, 4, 5, 6, 7, 8, c));
  return 0;
}
