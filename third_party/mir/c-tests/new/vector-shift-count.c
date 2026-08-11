typedef signed char v16qi __attribute__ ((vector_size (16)));
typedef unsigned char v16uqi __attribute__ ((vector_size (16)));
typedef short v8hi __attribute__ ((vector_size (16)));
typedef unsigned short v8uhi __attribute__ ((vector_size (16)));
typedef int v4si __attribute__ ((vector_size (16)));
typedef unsigned int v4ui __attribute__ ((vector_size (16)));
typedef long long v2di __attribute__ ((vector_size (16)));
typedef unsigned long long v2udi __attribute__ ((vector_size (16)));

int main (void) {
  v4si si = {1, 8, 16, 7};
  v4si sneg = {-8, -16, 32, -1};
  v4ui ui = {1u, 0x80000000u, 0xffffffffu, 16u};
  v4ui c4 = {1u, 2u, 3u, 1u};
  v4si sr;
  v4ui ur;

  sr = si << c4;
  if (sr[0] != 2 || sr[1] != 32 || sr[2] != 128 || sr[3] != 14) return 1;
  sr = sneg >> c4;
  if (sr[0] != -4 || sr[1] != -4 || sr[2] != 4 || sr[3] != -1) return 2;
  ur = ui >> c4;
  if (ur[0] != 0u || ur[1] != 0x20000000u || ur[2] != 0x1fffffffu || ur[3] != 8u)
    return 3;

  v8hi hi = {1, 2, 3, 4, 5, 6, 7, 8};
  v8hi hneg = {-2, -4, -8, -16, 32, 64, -128, 256};
  v8uhi hc = {0, 1, 2, 3, 1, 2, 3, 0};
  v8hi hr = hi << hc;
  if (hr[0] != 1 || hr[1] != 4 || hr[2] != 12 || hr[3] != 32) return 4;
  if (hr[4] != 10 || hr[5] != 24 || hr[6] != 56 || hr[7] != 8) return 5;
  hr = hneg >> hc;
  if (hr[0] != -2 || hr[1] != -2 || hr[2] != -2 || hr[3] != -2) return 6;
  if (hr[4] != 16 || hr[5] != 16 || hr[6] != -16 || hr[7] != 256) return 7;
  v8uhi uh = {1, 0x8000u, 0xffffu, 16u, 32u, 64u, 128u, 256u};
  v8uhi uhr = uh >> hc;
  if (uhr[0] != 1u || uhr[1] != 0x4000u || uhr[2] != 0x3fffu || uhr[3] != 2u)
    return 18;
  if (uhr[4] != 16u || uhr[5] != 16u || uhr[6] != 16u || uhr[7] != 256u) return 19;

  v16uqi uq = {1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 64, 128, 3, 5, 7, 9};
  v16uqi uc = {0, 1, 2, 3, 1, 2, 0, 1, 4, 5, 6, 7, 1, 2, 3, 0};
  v16uqi uqr = uq << uc;
  if (uqr[0] != 1 || uqr[1] != 4 || uqr[2] != 12 || uqr[3] != 32) return 8;
  if (uqr[4] != 10 || uqr[5] != 24 || uqr[8] != 0 || uqr[11] != 0) return 9;
  uqr = uq >> uc;
  if (uqr[0] != 1 || uqr[1] != 1 || uqr[2] != 0 || uqr[3] != 0) return 10;
  if (uqr[8] != 1 || uqr[9] != 1 || uqr[10] != 1 || uqr[11] != 1) return 11;

  v16qi sq = {-1, -2, -4, -8, 16, 32, -64, 127, -128, 64, -32, 8, -16, 4, -3, 2};
  v16qi sc = {0, 1, 2, 3, 1, 2, 3, 1, 7, 6, 5, 3, 4, 2, 1, 0};
  v16qi sqr = sq >> sc;
  if (sqr[0] != -1 || sqr[1] != -1 || sqr[2] != -1 || sqr[3] != -1) return 12;
  if (sqr[4] != 8 || sqr[5] != 8 || sqr[6] != -8 || sqr[7] != 63) return 13;
  if (sqr[8] != -1 || sqr[9] != 1 || sqr[10] != -1 || sqr[11] != 1) return 14;

  v2di di = {-8, 0x100000000LL};
  v2udi du = {0x1000000000000000ULL, 16ULL};
  v2udi c2 = {1ULL, 2ULL};
  v2di dr = di >> c2;
  v2udi dur;
  if (dr[0] != -4 || dr[1] != 0x40000000LL) return 15;
  dr = di << c2;
  if (dr[0] != -16 || dr[1] != 0x400000000LL) return 16;
  dur = du >> c2;
  if (dur[0] != 0x0800000000000000ULL || dur[1] != 4ULL) return 17;
  return 0;
}
