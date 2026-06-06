typedef int v4si __attribute__ ((vector_size (16)));
typedef int v1si __attribute__ ((vector_size (4)));
typedef int v2si __attribute__ ((vector_size (8)));
typedef int v8si __attribute__ ((vector_size (32)));
typedef unsigned int v2ui __attribute__ ((vector_size (8)));
typedef unsigned int v4ui __attribute__ ((vector_size (16)));
typedef unsigned int v8ui __attribute__ ((vector_size (32)));
typedef float v1sf __attribute__ ((vector_size (4)));
typedef float v2sf __attribute__ ((vector_size (8)));
typedef float v4sf __attribute__ ((vector_size (16)));
typedef float v8sf __attribute__ ((vector_size (32)));
typedef long long v2di __attribute__ ((vector_size (16)));
typedef unsigned long long v2udi __attribute__ ((vector_size (16)));
typedef long v2l __attribute__ ((vector_size (16)));
typedef long v4l __attribute__ ((vector_size (32)));
typedef double v2df __attribute__ ((__vector_size__ (16)));
typedef double v4df __attribute__ ((__vector_size__ (32)));
typedef signed char v1qi __attribute__ ((vector_size (1)));
typedef signed char v2qi __attribute__ ((vector_size (2)));
typedef unsigned char v2uqi __attribute__ ((vector_size (2)));
typedef signed char v4qi __attribute__ ((vector_size (4)));
typedef signed char v16qi __attribute__ ((vector_size (16)));
typedef unsigned char v16uqi __attribute__ ((vector_size (16)));
typedef short v1hi __attribute__ ((vector_size (2)));
typedef short v2hi __attribute__ ((vector_size (4)));
typedef short v4hi __attribute__ ((vector_size (8)));
typedef short v8hi __attribute__ ((vector_size (16)));
typedef unsigned short uint16x8_t __attribute__ ((vector_size (16)));

#if defined(__clang__) || defined(__MIRC__)
typedef int clang_i4 __attribute__ ((ext_vector_type (4)));
typedef int clang_i3 __attribute__ ((ext_vector_type (3)));
typedef unsigned char clang_uc3 __attribute__ ((ext_vector_type (3)));
typedef unsigned short clang_uh8 __attribute__ ((__ext_vector_type__ (8)));
typedef float clang_f4 __attribute__ ((ext_vector_type (4)));
#endif

static v4si g = {5, 6};

static v4si abi_add_i32 (v4si a, v4si b) {
  return a + b;
}

static int abi_sum_i32 (v4si v) {
  return v[0] + v[1] + v[2] + v[3];
}

static v4sf abi_mul_f32 (v4sf a, v4sf b) {
  return a * b;
}

static int abi_check_f32 (v4sf v) {
  if (v[0] != 3.0f) return 1;
  if (v[1] != -6.0f) return 2;
  if (v[2] != -3.0f) return 3;
  if (v[3] != 9.0f) return 4;
  return 0;
}

static v2df abi_add_f64 (v2df a, v2df b) {
  return a + b;
}

static int abi_check_f64 (v2df v) {
  if (v[0] != 4.75) return 1;
  if (v[1] != -2.0) return 2;
  return 0;
}

static v1qi abi_add_v8_i8 (v1qi a, v1qi b) {
  v1qi r = {a[0] + b[0]};
  return r;
}

static int abi_check_v8_i8 (v1qi v) {
  return v[0] == 42;
}

static v2qi abi_add_v16_i8 (v2qi a, v2qi b) {
  v2qi r = {a[0] + b[0], a[1] + b[1]};
  return r;
}

static int abi_sum_v16_i8 (v2qi v) {
  return v[0] * 10 + v[1];
}

static v2uqi abi_add_v16_u8 (v2uqi a, v2uqi b) {
  v2uqi r = {a[0] + b[0], a[1] + b[1]};
  return r;
}

static int abi_sum_v16_u8 (v2uqi v) {
  return v[0] * 10 + v[1];
}

static v1hi abi_add_v16_i16 (v1hi a, v1hi b) {
  v1hi r = {a[0] + b[0]};
  return r;
}

static int abi_check_v16_i16 (v1hi v) {
  return v[0] == 1234;
}

static v1si abi_add_v32_i32 (v1si a, v1si b) {
  v1si r = {a[0] + b[0]};
  return r;
}

static int abi_check_v32_i32 (v1si v) {
  return v[0] == 42;
}

static v1sf abi_add_v32_f32 (v1sf a, v1sf b) {
  v1sf r = {a[0] + b[0]};
  return r;
}

static int abi_check_v32_f32 (v1sf v) {
  return v[0] == 7.5f;
}

static v2hi abi_add_v32_i16 (v2hi a, v2hi b) {
  v2hi r = {a[0] + b[0], a[1] + b[1]};
  return r;
}

static int abi_sum_v32_i16 (v2hi v) {
  return v[0] + v[1];
}

static v4qi abi_add_v32_i8 (v4qi a, v4qi b) {
  v4qi r = {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]};
  return r;
}

static int abi_sum_v32_i8 (v4qi v) {
  return v[0] + v[1] + v[2] + v[3];
}

static v2si abi_add_v64_i32 (v2si a, v2si b) {
  v2si r = {a[0] + b[0], a[1] + b[1]};
  return r;
}

static int abi_sum_v64_i32 (v2si v) {
  return v[0] * 10 + v[1];
}

static v2sf abi_add_v64_f32 (v2sf a, v2sf b) {
  v2sf r = {a[0] + b[0], a[1] + b[1]};
  return r;
}

static int abi_check_v64_f32 (v2sf v) {
  if (v[0] != 4.5f) return 1;
  if (v[1] != 6.5f) return 2;
  return 0;
}

static v4hi abi_add_v64_i16 (v4hi a, v4hi b) {
  v4hi r = {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]};
  return r;
}

static int abi_sum_v64_i16 (v4hi v) {
  return v[0] * 1000 + v[1] * 100 + v[2] * 10 + v[3];
}

int main (void) {
  v4si a = {1, 2, 3, 4};
  v4si b = a;
  v4si c;
  v4si m = {0x0f, 0xf0, 0x33, 0x55};
  v4si n = {0x33, 0x55, 0x0f, 0xf0};
  v4si sh = {1, 2, 1, 3};
  v4si shuffle1 = {-1, -2, 1, 0};
  v4si shuffle2 = {0, 5, -2, 7};
  v4si pos = {1, 8, 16, 7};
  v4si neg = {-1, 0, 1, 2};
  v4si negsh = {-8, -16, 8, -1};
  v4si md_a = {6, -9, 20, -21};
  v4si md_b = {2, 3, -5, -4};
  v4si zero = {0, 0, 0, 0};
  v4ui ua = {1u, 2u, 3u, 4u};
  v4ui ub = {1u, 7u, 3u, 9u};
  v4ui ush = {1u, 1u, 4u, 2u};
  v4ui ushuffle = {8u, 9u, 0xffffffffu, 3u};
  v4ui usrc = {1u, 0x80000000u, 0xffffffffu, 16u};
  v4ui ux = {0u, 1u, 0x80000000u, 0xffffffffu};
  v4ui uy = {1u, 1u, 0x7fffffffu, 0xffffffffu};
  v4ui umd_a = {20u, 21u, 0xfffffff0u, 9u};
  v4ui umd_b = {3u, 4u, 8u, 2u};
  v4ui uc;
  v4si abi_ia = {1, 2, 3, 4};
  v4si abi_ib = {10, 20, 30, 40};
  v4si abi_ir;
  v2sf sf;
  v2sf nfa = {1.0f, -2.0f};
  v2sf nfb = {2.5f, 1.0f};
  v4sf vf;
  v4sf fa = {1.5f, -2.0f, 3.0f, -4.5f};
  v4sf fb = {0.5f, 4.0f, -1.0f, -1.5f};
  v4sf abi_fa = {1.5f, -2.0f, 3.0f, -4.5f};
  v4sf abi_fb = {2.0f, 3.0f, -1.0f, -2.0f};
  v4sf abi_fr;
  v8sf wfa = {1.0f, 2.0f, -3.0f, 4.0f, 5.0f, -6.0f, 7.0f, 8.0f};
  v8sf wfb = {0.5f, -1.0f, -3.0f, 2.0f, 10.0f, -12.0f, 1.0f, 16.0f};
  v8sf wvf;
  v2di li = {-3, 4};
  v2di li2;
  v2di li_add_a = {0x100000000LL, -9};
  v2di li_add_b = {5, -4};
  v2di li_add_c;
  v2udi uli_add_a = {0xfffffffffffffff0ULL, 20ULL};
  v2udi uli_add_b = {0x20ULL, 7ULL};
  v2udi uli_add_c;
  v2di li_shift_a = {0x100000000LL, 7};
  v2di li_shift_neg = {-8, 0x100000000LL};
  v2di li_shift_c;
  v2udi uli_shift_a = {0x1000000000000000ULL, 16ULL};
  v2udi uli_shift_c;
  v2di li_cmp_a = {0x100000000LL, -7};
  v2di li_cmp_b = {0x100000000LL, 9};
  v2di li_cmp_c;
  v2udi uli_cmp_a = {0xfffffffffffffff0ULL, 20ULL};
  v2udi uli_cmp_b = {0xfffffffffffffff0ULL, 7ULL};
  v2udi uli_cmp_c;
  v2df d = {1.25, 2.5};
  v2df e = {3.5, 4.5};
  v2si cv_i = {-1, 3};
  v2ui cv_u;
  v2sf cv_f;
  v2si cv_back;
  v2ui cast_u;
  v2si cast_i;
  v2sf cast_f;
  v4si cv_wi = {-1, 0, 1, 2};
  v4df cv_wd;
  v2si ni2a = {1, -4};
  v2si ni2b = {7, -2};
  v2si ni2c;
  v2ui nu2a = {0u, 0xffffffffu};
  v2ui nu2b = {1u, 0x7fffffffu};
  v2ui nu2c;
  v4hi nh4a = {1000, -2000, 7, -8};
  v4hi nh4b = {24, 3, -9, -8};
  v4hi nh4c;
  v4qi nq4a = {1, -2, 3, -4};
  v4qi nq4b = {1, 5, -3, -4};
  v4qi nq4c;
  v8si nw8a = {1, 2, 3, 4, 5, 6, 7, 8};
  v8si nw8b = {9, 10, 11, 12, 13, 14, 15, 16};
  v8si nw8c;
  v8si cast_wsrc = {-1, 0, 1, 2, 3, 4, 5, 6};
  v8ui cast_wu;
  v8si cast_wi;
  v8sf cast_wf;
  v2si scalar_cast_i64;
  v2sf scalar_cast_f64;
  v1si scalar_cast_i32;
  v4qi scalar_cast_i8x4;
  long long scalar_bits64;
  int scalar_bits32;
  v2df df_a = {1.25, -2.5};
  v2df df_b = {3.5, -0.5};
  v2df df_c;
  v2df abi_da = {1.25, -2.5};
  v2df abi_db = {3.5, 0.5};
  v2df abi_dr;
  v1qi abi_v8_ca = {12};
  v1qi abi_v8_cb = {30};
  v1qi abi_v8_cr;
  v2qi abi_v16_ca = {1, 2};
  v2qi abi_v16_cb = {10, 20};
  v2qi abi_v16_cr;
  v2uqi abi_v16_uca = {3, 4};
  v2uqi abi_v16_ucb = {5, 6};
  v2uqi abi_v16_ucr;
  v1hi abi_v16_ha = {1000};
  v1hi abi_v16_hb = {234};
  v1hi abi_v16_hr;
  v1si abi_v32_ia = {12};
  v1si abi_v32_ib = {30};
  v1si abi_v32_ir;
  v1sf abi_v32_fa = {2.5f};
  v1sf abi_v32_fb = {5.0f};
  v1sf abi_v32_fr;
  v2hi abi_v32_ha = {100, 200};
  v2hi abi_v32_hb = {1, 2};
  v2hi abi_v32_hr;
  v4qi abi_v32_ca = {1, 2, 3, 4};
  v4qi abi_v32_cb = {10, 20, 30, 40};
  v4qi abi_v32_cr;
  v2si abi_v64_ia = {3, 4};
  v2si abi_v64_ib = {5, 6};
  v2si abi_v64_ir;
  v2sf abi_v64_fa = {1.5f, 2.5f};
  v2sf abi_v64_fb = {3.0f, 4.0f};
  v2sf abi_v64_fr;
  v4hi abi_v64_ha = {1, 2, 3, 4};
  v4hi abi_v64_hb = {5, 6, 7, 8};
  v4hi abi_v64_hr;
  v2l dm;
  v4df wdf_a = {1.0, -2.0, 3.5, -4.5};
  v4df wdf_b = {0.5, -1.0, 7.0, -1.5};
  v4df wdf_c;
  v4l wdm;
  v2di dshuffle = {-1, 0};
  v8hi hia = {1, -2, 300, -400, 5000, -6000, 32760, -32768};
  v8hi hib = {2, -3, -200, 100, -20, 30, 10, -1};
  v8hi himask = {0, 9, 2, 11, 4, 13, 6, 15};
  v2si small;
  v8si wide;
  v2si a2 = {11, 22};
  v2si b2 = {33, 44};
  v2si m2 = {0, 3};
  v8si a8 = {1, 2, 3, 4, 5, 6, 7, 8};
  v8si b8 = {9, 10, 11, 12, 13, 14, 15, 16};
  v8si m8 = {0, 9, 7, 15, 3, 11, 4, 12};
  v4hi hsmall;
  v8hi hc;
  v8hi hpos = {1, 2, 3, 4, 5, 6, 7, 8};
  uint16x8_t uha = {1, 2, 65000, 65535, 128, 256, 1024, 32768};
  uint16x8_t uhb = {3, 2, 8, 5, 2, 4, 8, 16};
  uint16x8_t uhsh = {1, 2, 3, 1, 4, 0, 2, 3};
  uint16x8_t uhmask = {7, 6, 5, 4, 3, 2, 1, 0};
  uint16x8_t uhc;
  v16qi i8a = {1, -2, 3, -4, 5, -6, 7, -8, 9, -10, 11, -12, 13, -14, 15, -16};
  v16qi i8b = {1, 2, -3, -4, 5, 6, -7, -8, 9, 10, -11, -12, 13, 14, -15, -16};
  v16qi i8c;
  v16uqi u8a = {1, 2, 250, 255, 16, 32, 64, 128, 3, 4, 5, 6, 7, 8, 9, 10};
  v16uqi u8b = {2, 2, 6, 4, 1, 2, 4, 8, 3, 5, 7, 9, 11, 13, 15, 17};
  v16uqi u8c;
  v4si inc_i = {1, -2, 3, -4};
  v4si inc_ir;
  v8si inc_wi = {1, -2, 3, -4, 5, -6, 7, -8};
  v8si inc_wir;
  v16uqi inc_u = {0, 1, 2, 3, 4, 5, 250, 251, 252, 253, 254, 255, 10, 11, 12, 13};
  v16uqi inc_ur;
  v4sf inc_f = {1.0f, -2.0f, 3.5f, -4.5f};
  v4sf inc_fr;
  v8sf inc_wf = {1.0f, -2.0f, 3.5f, -4.5f, 5.0f, -6.0f, 7.0f, -8.0f};
  v8sf inc_wfr;
#if defined(__clang__) || defined(__MIRC__)
  clang_i4 ext_a = {1, -2, 3, -4};
  clang_i4 ext_b = {5, 6, -7, -8};
  clang_i4 ext_c;
  clang_i3 ext3_a = {1, -2, 3};
  clang_i3 ext3_b = {5, 6, -7};
  clang_i3 ext3_c;
  clang_uc3 ext3_u = {250, 2, 7};
  clang_uc3 ext3_v = {6, 4, 9};
  clang_uc3 ext3_w;
  clang_uh8 ext_u = {1, 2, 65000, 65535, 128, 256, 1024, 32768};
  clang_uh8 ext_v = {3, 2, 8, 5, 2, 4, 8, 16};
  clang_uh8 ext_w;
  clang_f4 ext_fx = {1.5f, -2.0f, 3.0f, -4.5f};
  clang_f4 ext_fy;
#endif
  int s = 7;
  unsigned int u = 0xff;

  if (sizeof (v4si) != 16) return 1;
  if (_Alignof (v4si) != 16) return 2;
#if defined(__clang__) || defined(__MIRC__)
  if (sizeof (clang_i4) != 16 || _Alignof (clang_i4) != 16) return 251;
  ext_c = ext_a + ext_b;
  if (ext_c[0] != 6 || ext_c[1] != 4 || ext_c[2] != -4 || ext_c[3] != -12)
    return 252;
  ext_w = ext_u + ext_v;
  if (ext_w[0] != 4 || ext_w[1] != 4 || ext_w[2] != 65008 || ext_w[3] != 4)
    return 253;
  ext_fy = ext_fx * 2.0f;
  if (ext_fy[0] != 3.0f || ext_fy[1] != -4.0f || ext_fy[2] != 6.0f
      || ext_fy[3] != -9.0f)
    return 254;
  if (sizeof (clang_i3) != 16 || _Alignof (clang_i3) != 16) return 255;
  if (sizeof (clang_uc3) != 4 || _Alignof (clang_uc3) != 4) return 255;
  if (__builtin_vectorelements (clang_i3) != 3 || __builtin_vectorelements (ext3_u) != 3)
    return 255;
  ext3_c = ext3_a + ext3_b;
  if (ext3_c[0] != 6 || ext3_c[1] != 4 || ext3_c[2] != -4) return 255;
  ext3_w = ext3_u + ext3_v;
  if (ext3_w[0] != 0 || ext3_w[1] != 6 || ext3_w[2] != 16) return 255;
#endif
  if (g[0] != 5 || g[1] != 6 || g[2] != 0 || g[3] != 0) return 3;
  b[0] = 9;
  if (b[0] != 9 || b[1] != 2 || b[3] != 4) return 4;
  if (d[0] != 1.25 || d[1] != 2.5) return 5;
  c = a + b;
  if (c[0] != 10 || c[1] != 4 || c[2] != 6 || c[3] != 8) return 6;
  c = b - a;
  if (c[0] != 8 || c[1] != 0 || c[2] != 0 || c[3] != 0) return 7;
  c = m & n;
  if (c[0] != 3 || c[1] != 0x50 || c[2] != 3 || c[3] != 0x50) return 8;
  c = m | n;
  if (c[0] != 0x3f || c[1] != 0xf5 || c[2] != 0x3f || c[3] != 0xf5) return 9;
  c = m ^ n;
  if (c[0] != 0x3c || c[1] != 0xa5 || c[2] != 0x3c || c[3] != 0xa5) return 10;
  c = a;
  c += b;
  c -= a;
  c &= m;
  c |= n;
  c ^= m;
  if (c[0] != 0x34 || c[1] != 0xa5 || c[2] != 0x3c || c[3] != 0xa1) return 11;
  c = a + 1;
  if (c[0] != 2 || c[1] != 3 || c[2] != 4 || c[3] != 5) return 12;
  c = 2 + a;
  if (c[0] != 3 || c[1] != 4 || c[2] != 5 || c[3] != 6) return 13;
  c = b - 3;
  if (c[0] != 6 || c[1] != -1 || c[2] != 0 || c[3] != 1) return 14;
  c = 10 - a;
  if (c[0] != 9 || c[1] != 8 || c[2] != 7 || c[3] != 6) return 15;
  c = m & 0x0f;
  if (c[0] != 0x0f || c[1] != 0 || c[2] != 3 || c[3] != 5) return 16;
  c = 0xf0 | a;
  if (c[0] != 0xf1 || c[1] != 0xf2 || c[2] != 0xf3 || c[3] != 0xf4) return 17;
  c = 0xff ^ a;
  if (c[0] != 0xfe || c[1] != 0xfd || c[2] != 0xfc || c[3] != 0xfb) return 18;
  c = a + s;
  if (c[0] != 8 || c[1] != 9 || c[2] != 10 || c[3] != 11) return 19;
  inc_ir = inc_i++;
  if (inc_ir[0] != 1 || inc_ir[1] != -2 || inc_i[0] != 2 || inc_i[1] != -1)
    return 241;
  inc_ir = ++inc_i;
  if (inc_ir[0] != 3 || inc_ir[1] != 0 || inc_i[0] != 3 || inc_i[1] != 0) return 242;
  inc_ir = inc_i--;
  if (inc_ir[2] != 5 || inc_ir[3] != -2 || inc_i[2] != 4 || inc_i[3] != -3)
    return 243;
  inc_ir = --inc_i;
  if (inc_ir[2] != 3 || inc_ir[3] != -4 || inc_i[2] != 3 || inc_i[3] != -4)
    return 244;
  inc_ur = ++inc_u;
  if (inc_ur[0] != 1 || inc_ur[6] != 251 || inc_ur[11] != 0) return 245;
  inc_ur = inc_u--;
  if (inc_ur[0] != 1 || inc_ur[11] != 0 || inc_u[0] != 0 || inc_u[11] != 255)
    return 246;
  inc_fr = ++inc_f;
  if (inc_fr[0] != 2.0f || inc_fr[1] != -1.0f || inc_fr[2] != 4.5f
      || inc_fr[3] != -3.5f)
    return 247;
  inc_fr = inc_f--;
  if (inc_fr[0] != 2.0f || inc_fr[1] != -1.0f || inc_f[0] != 1.0f || inc_f[1] != -2.0f)
    return 248;
  inc_wir = inc_wi++;
  if (inc_wir[0] != 1 || inc_wir[7] != -8 || inc_wi[0] != 2 || inc_wi[7] != -7)
    return 249;
  inc_wfr = ++inc_wf;
  if (inc_wfr[0] != 2.0f || inc_wfr[5] != -5.0f || inc_wfr[7] != -7.0f) return 250;
  c = u ^ a;
  if (c[0] != 0xfe || c[1] != 0xfd || c[2] != 0xfc || c[3] != 0xfb) return 20;
  c = a;
  c += 1L;
  c ^= 0xffffffffUL;
  if (c[0] != -3 || c[1] != -4 || c[2] != -5 || c[3] != -6) return 21;
  c = +a;
  if (c[0] != 1 || c[1] != 2 || c[2] != 3 || c[3] != 4) return 22;
  c = -a;
  if (c[0] != -1 || c[1] != -2 || c[2] != -3 || c[3] != -4) return 23;
  c = ~a;
  if (c[0] != -2 || c[1] != -3 || c[2] != -4 || c[3] != -5) return 24;
  c = a == b;
  if (c[0] != 0 || c[1] != -1 || c[2] != -1 || c[3] != -1) return 25;
  c = a != b;
  if (c[0] != -1 || c[1] != 0 || c[2] != 0 || c[3] != 0) return 26;
  c = a < b;
  if (c[0] != -1 || c[1] != 0 || c[2] != 0 || c[3] != 0) return 27;
  c = a <= b;
  if (c[0] != -1 || c[1] != -1 || c[2] != -1 || c[3] != -1) return 28;
  c = b > a;
  if (c[0] != -1 || c[1] != 0 || c[2] != 0 || c[3] != 0) return 29;
  c = b >= a;
  if (c[0] != -1 || c[1] != -1 || c[2] != -1 || c[3] != -1) return 30;
  c = a == 2;
  if (c[0] != 0 || c[1] != -1 || c[2] != 0 || c[3] != 0) return 31;
  c = 3 < a;
  if (c[0] != 0 || c[1] != 0 || c[2] != 0 || c[3] != -1) return 32;
  c = neg < zero;
  if (c[0] != -1 || c[1] != 0 || c[2] != 0 || c[3] != 0) return 33;
  uc = ua == ub;
  if (uc[0] != ~0u || uc[1] != 0u || uc[2] != ~0u || uc[3] != 0u) return 34;
  uc = ua != ub;
  if (uc[0] != 0u || uc[1] != ~0u || uc[2] != 0u || uc[3] != ~0u) return 35;
  uc = ua == 3u;
  if (uc[0] != 0u || uc[1] != 0u || uc[2] != ~0u || uc[3] != 0u) return 36;
  uc = 2u != ua;
  if (uc[0] != ~0u || uc[1] != 0u || uc[2] != ~0u || uc[3] != ~0u) return 37;
  uc = ux < uy;
  if (uc[0] != ~0u || uc[1] != 0u || uc[2] != 0u || uc[3] != 0u) return 38;
  uc = ux > uy;
  if (uc[0] != 0u || uc[1] != 0u || uc[2] != ~0u || uc[3] != 0u) return 39;
  uc = ux <= uy;
  if (uc[0] != ~0u || uc[1] != ~0u || uc[2] != 0u || uc[3] != ~0u) return 40;
  uc = ux >= uy;
  if (uc[0] != 0u || uc[1] != ~0u || uc[2] != ~0u || uc[3] != ~0u) return 41;
  uc = ux > 0x7fffffffu;
  if (uc[0] != 0u || uc[1] != 0u || uc[2] != ~0u || uc[3] != ~0u) return 42;
  uc = 1u < ux;
  if (uc[0] != 0u || uc[1] != 0u || uc[2] != ~0u || uc[3] != ~0u) return 43;
  c = pos << sh;
  if (c[0] != 2 || c[1] != 32 || c[2] != 32 || c[3] != 56) return 44;
  c = negsh >> sh;
  if (c[0] != -4 || c[1] != -4 || c[2] != 4 || c[3] != -1) return 45;
  c = pos << 2;
  if (c[0] != 4 || c[1] != 32 || c[2] != 64 || c[3] != 28) return 46;
  c = 1 << sh;
  if (c[0] != 2 || c[1] != 4 || c[2] != 2 || c[3] != 8) return 47;
  c = pos;
  c <<= sh;
  if (c[0] != 2 || c[1] != 32 || c[2] != 32 || c[3] != 56) return 48;
  c = negsh;
  c >>= sh;
  if (c[0] != -4 || c[1] != -4 || c[2] != 4 || c[3] != -1) return 49;
  uc = usrc >> ush;
  if (uc[0] != 0u || uc[1] != 0x40000000u || uc[2] != 0x0fffffffu || uc[3] != 4u) return 50;
  uc = usrc << 1u;
  if (uc[0] != 2u || uc[1] != 0u || uc[2] != 0xfffffffeu || uc[3] != 32u) return 51;
  uc = usrc;
  uc >>= ush;
  if (uc[0] != 0u || uc[1] != 0x40000000u || uc[2] != 0x0fffffffu || uc[3] != 4u) return 52;
  c = pos << ush;
  if (c[0] != 2 || c[1] != 16 || c[2] != 256 || c[3] != 28) return 255;
  uc = usrc >> sh;
  if (uc[0] != 0u || uc[1] != 0x20000000u || uc[2] != 0x7fffffffu || uc[3] != 2u)
    return 255;
  c = pos;
  c <<= ush;
  if (c[0] != 2 || c[1] != 16 || c[2] != 256 || c[3] != 28) return 255;
  c = md_a * md_b;
  if (c[0] != 12 || c[1] != -27 || c[2] != -100 || c[3] != 84) return 53;
  c = md_a / md_b;
  if (c[0] != 3 || c[1] != -3 || c[2] != -4 || c[3] != 5) return 54;
  c = md_a % md_b;
  if (c[0] != 0 || c[1] != 0 || c[2] != 0 || c[3] != -1) return 55;
  c = md_a;
  c *= 2;
  c /= md_b;
  c %= 5;
  if (c[0] != 1 || c[1] != -1 || c[2] != -3 || c[3] != 0) return 56;
  uc = umd_a * umd_b;
  if (uc[0] != 60u || uc[1] != 84u || uc[2] != 0xffffff80u || uc[3] != 18u) return 57;
  uc = umd_a / umd_b;
  if (uc[0] != 6u || uc[1] != 5u || uc[2] != 0x1ffffffeu || uc[3] != 4u) return 58;
  uc = umd_a % umd_b;
  if (uc[0] != 2u || uc[1] != 1u || uc[2] != 0u || uc[3] != 1u) return 59;
  uc = umd_a;
  uc *= 3u;
  uc /= umd_b;
  uc %= 7u;
  if (uc[0] != 6u || uc[1] != 1u || uc[2] != 5u || uc[3] != 6u) return 60;
  uc = (v4ui) neg;
  if (uc[0] != ~0u || uc[1] != 0u || uc[2] != 1u || uc[3] != 2u) return 61;
  c = (v4si) uc;
  if (c[0] != -1 || c[1] != 0 || c[2] != 1 || c[3] != 2) return 62;
  uc = __builtin_convertvector (neg, v4ui);
  if (uc[0] != ~0u || uc[1] != 0u || uc[2] != 1u || uc[3] != 2u) return 63;
  vf = __builtin_convertvector (neg, v4sf);
  if (vf[0] != -1.0f || vf[1] != 0.0f || vf[2] != 1.0f || vf[3] != 2.0f) return 64;
  c = __builtin_convertvector (vf, v4si);
  if (c[0] != -1 || c[1] != 0 || c[2] != 1 || c[3] != 2) return 65;
  c = __builtin_shufflevector (md_a, md_b, 0, 5, 2, 7);
  if (c[0] != 6 || c[1] != 3 || c[2] != 20 || c[3] != -4) return 66;
  d = __builtin_convertvector (li, v2df);
  if (d[0] != -3.0 || d[1] != 4.0) return 67;
  li2 = __builtin_convertvector (d, v2di);
  if (li2[0] != -3 || li2[1] != 4) return 68;
  d = __builtin_shufflevector (d, d, 1, 0);
  if (d[0] != 4.0 || d[1] != -3.0) return 69;
  c = __builtin_shuffle (a, shuffle1);
  if (c[0] != 4 || c[1] != 3 || c[2] != 2 || c[3] != 1) return 70;
  c = __builtin_shuffle (md_a, md_b, shuffle2);
  if (c[0] != 6 || c[1] != 3 || c[2] != -5 || c[3] != -4) return 71;
  uc = __builtin_shuffle (ua, ub, ushuffle);
  if (uc[0] != 1u || uc[1] != 2u || uc[2] != 9u || uc[3] != 4u) return 72;
  d = __builtin_shuffle (d, e, dshuffle);
  if (d[0] != 4.5 || d[1] != 4.0) return 73;
#ifdef __MIRC__
  if (__builtin_vectorelements (uint16x8_t) != 8) return 74;
  if (__builtin_vectorelements (uha) != 8) return 75;
#endif
  hc = hia + hib;
  if (hc[0] != 3 || hc[1] != -5 || hc[2] != 100 || hc[3] != -300) return 76;
  hc = hia - hib;
  if (hc[0] != -1 || hc[1] != 1 || hc[2] != 500 || hc[3] != -500) return 77;
  hc = hia + 5;
  if (hc[0] != 6 || hc[1] != 3 || hc[2] != 305 || hc[3] != -395) return 78;
  hc = 7 - hia;
  if (hc[0] != 6 || hc[1] != 9 || hc[2] != -293 || hc[3] != 407) return 79;
  hc = hia & hib;
  if (hc[0] != 0 || hc[1] != -4 || hc[2] != 296 || hc[3] != 96) return 80;
  hc = hia | hib;
  if (hc[0] != 3 || hc[1] != -1 || hc[2] != -196 || hc[3] != -396) return 81;
  hc = hia ^ hib;
  if (hc[0] != 3 || hc[1] != 3 || hc[2] != -492 || hc[3] != -492) return 82;
  hc = -hia;
  if (hc[0] != -1 || hc[1] != 2 || hc[2] != -300 || hc[3] != 400) return 83;
  hc = ~hia;
  if (hc[0] != -2 || hc[1] != 1 || hc[2] != -301 || hc[3] != 399) return 84;
  hc = hia < hib;
  if (hc[0] != -1 || hc[1] != 0 || hc[2] != 0 || hc[3] != -1 || hc[7] != -1) return 85;
  hc = __builtin_shufflevector (hia, hib, 0, 9, 2, 11, 4, 13, 6, 15);
  if (hc[0] != 1 || hc[1] != -3 || hc[2] != 300 || hc[3] != 100) return 86;
  hc = __builtin_shuffle (hia, hib, himask);
  if (hc[0] != 1 || hc[1] != -3 || hc[2] != 300 || hc[3] != 100) return 87;
  hc = hia;
  hc += 2;
  hc ^= 0xff;
  if (hc[0] != 252 || hc[1] != 255 || hc[2] != 465 || hc[3] != -371) return 88;
  uhc = uha + uhb;
  if (uhc[0] != 4 || uhc[1] != 4 || uhc[2] != 65008 || uhc[3] != 4) return 89;
  uhc = uha >> 2;
  if (uhc[0] != 0 || uhc[1] != 0 || uhc[2] != 16250 || uhc[3] != 16383) return 90;
  uhc = uha < uhb;
  if (uhc[0] != 65535 || uhc[1] != 0 || uhc[2] != 0 || uhc[3] != 0) return 91;
  uhc = __builtin_shufflevector (uha, uhb, 0, 9, 2, 11, 4, 13, 6, 15);
  if (uhc[0] != 1 || uhc[1] != 2 || uhc[2] != 65000 || uhc[3] != 5) return 92;
  uhc = __builtin_shuffle (uha, uhb, uhmask);
  if (uhc[0] != 32768 || uhc[1] != 1024 || uhc[2] != 256 || uhc[3] != 128) return 93;
  i8c = i8a + i8b;
  if (i8c[0] != 2 || i8c[1] != 0 || i8c[2] != 0 || i8c[3] != -8) return 94;
  i8c = i8a < i8b;
  if (i8c[0] != 0 || i8c[1] != -1 || i8c[2] != 0 || i8c[3] != 0) return 95;
  u8c = u8a + u8b;
  if (u8c[0] != 3 || u8c[1] != 4 || u8c[2] != 0 || u8c[3] != 3) return 96;
  u8c = u8a > u8b;
  if (u8c[0] != 0 || u8c[1] != 0 || u8c[2] != 255 || u8c[3] != 255) return 97;
  i8c = i8a << 1;
  if (i8c[0] != 2 || i8c[1] != -4 || i8c[2] != 6 || i8c[3] != -8) return 232;
  i8c = i8a >> 1;
  if (i8c[0] != 0 || i8c[1] != -1 || i8c[2] != 1 || i8c[3] != -2) return 233;
  i8c = i8a;
  i8c >>= 2;
  if (i8c[0] != 0 || i8c[1] != -1 || i8c[2] != 0 || i8c[3] != -1) return 234;
  u8c = u8a << 1;
  if (u8c[0] != 2 || u8c[1] != 4 || u8c[2] != 244 || u8c[3] != 254) return 235;
  u8c = u8a >> 1;
  if (u8c[0] != 0 || u8c[1] != 1 || u8c[2] != 125 || u8c[3] != 127) return 236;
  i8c = i8a * i8b;
  if (i8c[0] != 1 || i8c[1] != -4 || i8c[2] != -9 || i8c[3] != 16 || i8c[13] != 60
      || i8c[14] != 31)
    return 237;
  i8c = i8a;
  i8c *= i8b;
  if (i8c[0] != 1 || i8c[1] != -4 || i8c[2] != -9 || i8c[3] != 16 || i8c[13] != 60
      || i8c[14] != 31)
    return 238;
  u8c = u8a * u8b;
  if (u8c[0] != 2 || u8c[1] != 4 || u8c[2] != 220 || u8c[3] != 252 || u8c[6] != 0
      || u8c[7] != 0 || u8c[15] != 170)
    return 239;
  u8c = u8a;
  u8c *= u8b;
  if (u8c[0] != 2 || u8c[1] != 4 || u8c[2] != 220 || u8c[3] != 252 || u8c[6] != 0
      || u8c[7] != 0 || u8c[15] != 170)
    return 240;
  small = __builtin_shufflevector (a, b, 0, 5);
  if (sizeof (small) != 8 || small[0] != 1 || small[1] != 2) return 98;
  wide = __builtin_shufflevector (a, b, 0, 4, 1, 5, 2, 6, 3, 7);
  if (sizeof (wide) != 32 || wide[0] != 1 || wide[1] != 9 || wide[6] != 4 || wide[7] != 4)
    return 99;
  hsmall = __builtin_shufflevector (hia, hib, 0, 9, 2, 11);
  if (sizeof (hsmall) != 8 || hsmall[0] != 1 || hsmall[1] != -3 || hsmall[2] != 300
      || hsmall[3] != 100)
    return 100;
  c = __builtin_shufflevector (a2, b2, 0, 2, 1, 3);
  if (c[0] != 11 || c[1] != 33 || c[2] != 22 || c[3] != 44) return 101;
  c = __builtin_shufflevector (a8, b8, 0, 8, 7, 15);
  if (c[0] != 1 || c[1] != 9 || c[2] != 8 || c[3] != 16) return 102;
  c = __builtin_shufflevector (a2, b, 0, 2, 5, 1);
  if (c[0] != 11 || c[1] != 9 || c[2] != 4 || c[3] != 22) return 202;
  small = __builtin_shuffle (a2, b2, m2);
  if (small[0] != 11 || small[1] != 44) return 103;
  wide = __builtin_shuffle (a8, b8, m8);
  if (wide[0] != 1 || wide[1] != 10 || wide[2] != 8 || wide[3] != 16
      || wide[4] != 4 || wide[5] != 12 || wide[6] != 5 || wide[7] != 13)
    return 104;
  vf = fa + fb;
  if (vf[0] != 2.0f || vf[1] != 2.0f || vf[2] != 2.0f || vf[3] != -6.0f) return 105;
  vf = fa * fb;
  if (vf[0] != 0.75f || vf[1] != -8.0f || vf[2] != -3.0f || vf[3] != 6.75f) return 106;
  vf = -fa;
  if (vf[0] != -1.5f || vf[1] != 2.0f || vf[2] != -3.0f || vf[3] != 4.5f) return 107;
  vf = 2.0f + fa;
  if (vf[0] != 3.5f || vf[1] != 0.0f || vf[2] != 5.0f || vf[3] != -2.5f) return 108;
  c = fa < fb;
  if (c[0] != 0 || c[1] != -1 || c[2] != 0 || c[3] != -1) return 109;
  vf = fa;
  vf += fb;
  vf /= 2.0f;
  if (vf[0] != 1.0f || vf[1] != 1.0f || vf[2] != 1.0f || vf[3] != -3.0f) return 110;
  df_c = df_a / df_b;
  if (df_c[0] != 1.25 / 3.5 || df_c[1] != 5.0) return 111;
  dm = df_a >= df_b;
  if (dm[0] != 0 || dm[1] != 0) return 112;
  sf = nfa + nfb;
  if (sf[0] != 3.5f || sf[1] != -1.0f) return 113;
  sf = nfa * 2.0f;
  if (sf[0] != 2.0f || sf[1] != -4.0f) return 114;
  sf = -nfa;
  if (sf[0] != -1.0f || sf[1] != 2.0f) return 115;
  small = nfa < nfb;
  if (small[0] != -1 || small[1] != -1) return 116;
  wvf = wfa - wfb;
  if (wvf[0] != 0.5f || wvf[1] != 3.0f || wvf[2] != 0.0f || wvf[3] != 2.0f
      || wvf[4] != -5.0f || wvf[5] != 6.0f || wvf[6] != 6.0f || wvf[7] != -8.0f)
    return 117;
  wvf = wfa / 2.0f;
  if (wvf[0] != 0.5f || wvf[1] != 1.0f || wvf[2] != -1.5f || wvf[3] != 2.0f
      || wvf[4] != 2.5f || wvf[5] != -3.0f || wvf[6] != 3.5f || wvf[7] != 4.0f)
    return 118;
  wide = wfa >= wfb;
  if (wide[0] != -1 || wide[1] != -1 || wide[2] != -1 || wide[3] != -1
      || wide[4] != 0 || wide[5] != -1 || wide[6] != -1 || wide[7] != 0)
    return 119;
  wdf_c = wdf_a + wdf_b;
  if (wdf_c[0] != 1.5 || wdf_c[1] != -3.0 || wdf_c[2] != 10.5 || wdf_c[3] != -6.0)
    return 120;
  wdf_c = wdf_a / wdf_b;
  if (wdf_c[0] != 2.0 || wdf_c[1] != 2.0 || wdf_c[2] != 0.5 || wdf_c[3] != 3.0)
    return 121;
  wdm = wdf_a != wdf_b;
  if (wdm[0] != -1 || wdm[1] != -1 || wdm[2] != -1 || wdm[3] != -1) return 122;
  abi_ir = abi_add_i32 (abi_ia, abi_ib);
  if (abi_ir[0] != 11 || abi_ir[1] != 22 || abi_ir[2] != 33 || abi_ir[3] != 44) return 123;
  if (abi_sum_i32 (abi_ir) != 110) return 124;
  abi_fr = abi_mul_f32 (abi_fa, abi_fb);
  if (abi_check_f32 (abi_fr) != 0) return 125;
  abi_dr = abi_add_f64 (abi_da, abi_db);
  if (abi_check_f64 (abi_dr) != 0) return 126;
  abi_v8_cr = abi_add_v8_i8 (abi_v8_ca, abi_v8_cb);
  if (!abi_check_v8_i8 (abi_v8_cr)) return 226;
  abi_v16_cr = abi_add_v16_i8 (abi_v16_ca, abi_v16_cb);
  if (abi_v16_cr[0] != 11 || abi_v16_cr[1] != 22) return 227;
  if (abi_sum_v16_i8 (abi_v16_cr) != 132) return 228;
  abi_v16_ucr = abi_add_v16_u8 (abi_v16_uca, abi_v16_ucb);
  if (abi_v16_ucr[0] != 8 || abi_v16_ucr[1] != 10) return 229;
  if (abi_sum_v16_u8 (abi_v16_ucr) != 90) return 230;
  abi_v16_hr = abi_add_v16_i16 (abi_v16_ha, abi_v16_hb);
  if (!abi_check_v16_i16 (abi_v16_hr)) return 231;
  abi_v64_ir = abi_add_v64_i32 (abi_v64_ia, abi_v64_ib);
  if (abi_v64_ir[0] != 8 || abi_v64_ir[1] != 10) return 127;
  if (abi_sum_v64_i32 (abi_v64_ir) != 90) return 128;
  abi_v64_fr = abi_add_v64_f32 (abi_v64_fa, abi_v64_fb);
  if (abi_check_v64_f32 (abi_v64_fr) != 0) return 129;
  abi_v64_hr = abi_add_v64_i16 (abi_v64_ha, abi_v64_hb);
  if (abi_v64_hr[0] != 6 || abi_v64_hr[1] != 8 || abi_v64_hr[2] != 10
      || abi_v64_hr[3] != 12)
    return 130;
  if (abi_sum_v64_i16 (abi_v64_hr) != 6912) return 131;
  abi_v32_ir = abi_add_v32_i32 (abi_v32_ia, abi_v32_ib);
  if (!abi_check_v32_i32 (abi_v32_ir)) return 132;
  abi_v32_fr = abi_add_v32_f32 (abi_v32_fa, abi_v32_fb);
  if (!abi_check_v32_f32 (abi_v32_fr)) return 133;
  abi_v32_hr = abi_add_v32_i16 (abi_v32_ha, abi_v32_hb);
  if (abi_v32_hr[0] != 101 || abi_v32_hr[1] != 202) return 134;
  if (abi_sum_v32_i16 (abi_v32_hr) != 303) return 135;
  abi_v32_cr = abi_add_v32_i8 (abi_v32_ca, abi_v32_cb);
  if (abi_v32_cr[0] != 11 || abi_v32_cr[1] != 22 || abi_v32_cr[2] != 33
      || abi_v32_cr[3] != 44)
    return 136;
  if (abi_sum_v32_i8 (abi_v32_cr) != 110) return 137;
  vf = fa - fb;
  if (vf[0] != 1.0f || vf[1] != -6.0f || vf[2] != 4.0f || vf[3] != -3.0f) return 138;
  c = fa == fa;
  if (c[0] != -1 || c[1] != -1 || c[2] != -1 || c[3] != -1) return 139;
  c = fa != fb;
  if (c[0] != -1 || c[1] != -1 || c[2] != -1 || c[3] != -1) return 140;
  c = fa <= fb;
  if (c[0] != 0 || c[1] != -1 || c[2] != 0 || c[3] != -1) return 141;
  c = fa > fb;
  if (c[0] != -1 || c[1] != 0 || c[2] != -1 || c[3] != 0) return 142;
  c = fa >= fb;
  if (c[0] != -1 || c[1] != 0 || c[2] != -1 || c[3] != 0) return 143;
  df_c = df_a + df_b;
  if (df_c[0] != 4.75 || df_c[1] != -3.0) return 144;
  df_c = df_a - df_b;
  if (df_c[0] != -2.25 || df_c[1] != -2.0) return 145;
  df_c = df_a * df_b;
  if (df_c[0] != 4.375 || df_c[1] != 1.25) return 146;
  df_c = df_a / df_b;
  if (df_c[0] != 1.25 / 3.5 || df_c[1] != 5.0) return 147;
  dm = df_a == df_a;
  if (dm[0] != -1 || dm[1] != -1) return 148;
  dm = df_a != df_b;
  if (dm[0] != -1 || dm[1] != -1) return 149;
  dm = df_a < df_b;
  if (dm[0] != -1 || dm[1] != -1) return 150;
  dm = df_a <= df_b;
  if (dm[0] != -1 || dm[1] != -1) return 151;
  dm = df_b > df_a;
  if (dm[0] != -1 || dm[1] != -1) return 152;
  dm = df_b >= df_a;
  if (dm[0] != -1 || dm[1] != -1) return 153;
  i8c = i8a - i8b;
  if (i8c[0] != 0 || i8c[1] != -4 || i8c[2] != 6 || i8c[3] != 0) return 154;
  hc = hia;
  hc += hib;
  if (hc[0] != 3 || hc[1] != -5 || hc[2] != 100 || hc[3] != -300) return 155;
  hc -= hib;
  if (hc[0] != 1 || hc[1] != -2 || hc[2] != 300 || hc[3] != -400) return 156;
  i8c = i8a == i8b;
  if (i8c[0] != -1 || i8c[1] != 0 || i8c[2] != 0 || i8c[3] != -1) return 157;
  uhc = uha != uhb;
  if (uhc[0] != 65535 || uhc[1] != 0 || uhc[2] != 65535 || uhc[3] != 65535) return 158;
  hc = hia * hib;
  if (hc[0] != 2 || hc[1] != 6 || hc[2] != 5536 || hc[3] != 25536) return 159;
  hc = hia;
  hc *= hib;
  if (hc[0] != 2 || hc[1] != 6 || hc[2] != 5536 || hc[3] != 25536) return 160;
  uhc = uha * uhb;
  if (uhc[0] != 3 || uhc[1] != 4 || uhc[2] != 61248 || uhc[3] != 65531) return 161;
  hc = hia << 2;
  if (hc[0] != 4 || hc[1] != -8 || hc[2] != 1200 || hc[3] != -1600) return 162;
  hc = hia >> 2;
  if (hc[0] != 0 || hc[1] != -1 || hc[2] != 75 || hc[3] != -100) return 163;
  uhc = uha << 1;
  if (uhc[0] != 2 || uhc[1] != 4 || uhc[2] != 64464 || uhc[3] != 65534) return 164;
  hc = hia;
  hc <<= 1;
  if (hc[0] != 2 || hc[1] != -4 || hc[2] != 600 || hc[3] != -800) return 165;
  hc = hpos << uhsh;
  if (hc[0] != 2 || hc[1] != 8 || hc[2] != 24 || hc[3] != 8) return 255;
  if (hc[4] != 80 || hc[5] != 6 || hc[6] != 28 || hc[7] != 64) return 255;
  hpos <<= uhsh;
  if (hpos[0] != 2 || hpos[1] != 8 || hpos[2] != 24 || hpos[3] != 8) return 255;
  if (hpos[4] != 80 || hpos[5] != 6 || hpos[6] != 28 || hpos[7] != 64) return 255;
  uhc = uha;
  uhc >>= 1;
  if (uhc[0] != 0 || uhc[1] != 1 || uhc[2] != 32500 || uhc[3] != 32767) return 166;
  c = negsh >> 2;
  if (c[0] != -2 || c[1] != -4 || c[2] != 2 || c[3] != -1) return 167;
  uc = usrc >> 2u;
  if (uc[0] != 0u || uc[1] != 0x20000000u || uc[2] != 0x3fffffffu || uc[3] != 4u) return 168;
  c = pos;
  c <<= 1;
  if (c[0] != 2 || c[1] != 16 || c[2] != 32 || c[3] != 14) return 169;
  uc = usrc;
  uc >>= 1u;
  if (uc[0] != 0u || uc[1] != 0x40000000u || uc[2] != 0x7fffffffu || uc[3] != 8u) return 170;
  c = s > 0 ? a : b;
  if (c[0] != 1 || c[1] != 2 || c[2] != 3 || c[3] != 4) return 171;
  c = s < 0 ? a : b;
  if (c[0] != 9 || c[1] != 2 || c[2] != 3 || c[3] != 4) return 172;
  if ((s > 0 ? a : b)[2] != 3) return 173;
  cv_u = __builtin_convertvector (cv_i, v2ui);
  if (cv_u[0] != ~0u || cv_u[1] != 3u) return 174;
  cv_f = __builtin_convertvector (cv_i, v2sf);
  if (cv_f[0] != -1.0f || cv_f[1] != 3.0f) return 175;
  cv_back = __builtin_convertvector (cv_f, v2si);
  if (cv_back[0] != -1 || cv_back[1] != 3) return 176;
  cv_wd = __builtin_convertvector (cv_wi, v4df);
  if (cv_wd[0] != -1.0 || cv_wd[1] != 0.0 || cv_wd[2] != 1.0 || cv_wd[3] != 2.0) return 177;
  cast_u = (v2ui) cv_i;
  if (cast_u[0] != ~0u || cast_u[1] != 3u) return 178;
  cast_i = (v2si) cast_u;
  if (cast_i[0] != -1 || cast_i[1] != 3) return 179;
  cast_f = (v2sf) cv_i;
  if (((v2ui) cast_f)[0] != ~0u || ((v2ui) cast_f)[1] != 3u) return 180;
  cast_wu = (v8ui) cast_wsrc;
  if (cast_wu[0] != ~0u || cast_wu[1] != 0u || cast_wu[7] != 6u) return 181;
  cast_wi = (v8si) cast_wu;
  if (cast_wi[0] != -1 || cast_wi[1] != 0 || cast_wi[7] != 6) return 182;
  cast_wf = (v8sf) cast_wsrc;
  if (((v8ui) cast_wf)[0] != ~0u || ((v8ui) cast_wf)[7] != 6u) return 183;
  ni2c = ni2a + ni2b;
  if (ni2c[0] != 8 || ni2c[1] != -6) return 184;
  ni2c = ni2b - ni2a;
  if (ni2c[0] != 6 || ni2c[1] != 2) return 185;
  ni2c = ~ni2a;
  if (ni2c[0] != -2 || ni2c[1] != 3) return 186;
  ni2c = ni2a | ni2b;
  if (ni2c[0] != 7 || ni2c[1] != -2) return 187;
  ni2c = ni2a < ni2b;
  if (ni2c[0] != -1 || ni2c[1] != -1) return 188;
  nu2c = nu2a > nu2b;
  if (nu2c[0] != 0u || nu2c[1] != ~0u) return 189;
  nu2c = nu2b << 1u;
  if (nu2c[0] != 2u || nu2c[1] != 0xfffffffeu) return 190;
  nh4c = nh4a + nh4b;
  if (nh4c[0] != 1024 || nh4c[1] != -1997 || nh4c[2] != -2 || nh4c[3] != -16)
    return 191;
  nq4c = nq4a == nq4b;
  if (nq4c[0] != -1 || nq4c[1] != 0 || nq4c[2] != 0 || nq4c[3] != -1) return 192;
  nw8c = nw8a + nw8b;
  if (nw8c[0] != 10 || nw8c[1] != 12 || nw8c[2] != 14 || nw8c[3] != 16
      || nw8c[4] != 18 || nw8c[5] != 20 || nw8c[6] != 22 || nw8c[7] != 24)
    return 193;
  scalar_cast_i64 = (v2si) 0x0000000200000001LL;
  if (scalar_cast_i64[0] != 1 || scalar_cast_i64[1] != 2) return 194;
  scalar_bits64 = (long long) scalar_cast_i64;
  if (scalar_bits64 != 0x0000000200000001LL) return 195;
  scalar_cast_f64 = (v2sf) 0x400000003f800000LL;
  if (scalar_cast_f64[0] != 1.0f || scalar_cast_f64[1] != 2.0f) return 196;
  scalar_bits64 = (long long) scalar_cast_f64;
  if (scalar_bits64 != 0x400000003f800000LL) return 197;
  scalar_cast_i32 = (v1si) 42;
  if (scalar_cast_i32[0] != 42) return 198;
  scalar_bits32 = (int) scalar_cast_i32;
  if (scalar_bits32 != 42) return 199;
  scalar_cast_i8x4 = (v4qi) 0x04030201;
  if (scalar_cast_i8x4[0] != 1 || scalar_cast_i8x4[1] != 2 || scalar_cast_i8x4[2] != 3
      || scalar_cast_i8x4[3] != 4)
    return 200;
  scalar_bits32 = (int) scalar_cast_i8x4;
  if (scalar_bits32 != 0x04030201) return 201;
  li_add_c = li_add_a + li_add_b;
  if (li_add_c[0] != 0x100000005LL || li_add_c[1] != -13) return 203;
  li_add_c = li_add_a - li_add_b;
  if (li_add_c[0] != 0xfffffffbLL || li_add_c[1] != -5) return 204;
  li_add_c = li_add_a;
  li_add_c += li_add_b;
  if (li_add_c[0] != 0x100000005LL || li_add_c[1] != -13) return 205;
  li_add_c -= li_add_b;
  if (li_add_c[0] != 0x100000000LL || li_add_c[1] != -9) return 206;
  uli_add_c = uli_add_a + uli_add_b;
  if (uli_add_c[0] != 0x10ULL || uli_add_c[1] != 27ULL) return 207;
  uli_add_c = uli_add_b - uli_add_a;
  if (uli_add_c[0] != 0x30ULL || uli_add_c[1] != 0xfffffffffffffff3ULL) return 208;
  uli_add_c = uli_add_a;
  uli_add_c += uli_add_b;
  if (uli_add_c[0] != 0x10ULL || uli_add_c[1] != 27ULL) return 209;
  uli_add_c -= uli_add_b;
  if (uli_add_c[0] != 0xfffffffffffffff0ULL || uli_add_c[1] != 20ULL) return 210;
  li_shift_c = li_shift_a << 2;
  if (li_shift_c[0] != 0x400000000LL || li_shift_c[1] != 28) return 211;
  li_shift_c = li_shift_a;
  li_shift_c <<= 3;
  if (li_shift_c[0] != 0x800000000LL || li_shift_c[1] != 56) return 212;
  uli_shift_c = uli_shift_a << 1;
  if (uli_shift_c[0] != 0x2000000000000000ULL || uli_shift_c[1] != 32ULL) return 213;
  uli_shift_c = uli_shift_a;
  uli_shift_c <<= 2;
  if (uli_shift_c[0] != 0x4000000000000000ULL || uli_shift_c[1] != 64ULL) return 214;
  uli_shift_c = uli_shift_a >> 4;
  if (uli_shift_c[0] != 0x0100000000000000ULL || uli_shift_c[1] != 1ULL) return 215;
  uli_shift_c = uli_shift_a;
  uli_shift_c >>= 1;
  if (uli_shift_c[0] != 0x0800000000000000ULL || uli_shift_c[1] != 8ULL) return 216;
  li_shift_c = li_shift_neg >> 2;
  if (li_shift_c[0] != -2 || li_shift_c[1] != 0x40000000LL) return 217;
  li_shift_c = li_shift_neg;
  li_shift_c >>= 1;
  if (li_shift_c[0] != -4 || li_shift_c[1] != 0x80000000LL) return 218;
  li_shift_c = li_shift_neg >> 0;
  if (li_shift_c[0] != -8 || li_shift_c[1] != 0x100000000LL) return 225;
  li_cmp_c = li_cmp_a == li_cmp_b;
  if (li_cmp_c[0] != -1 || li_cmp_c[1] != 0) return 219;
  li_cmp_c = li_cmp_a != li_cmp_b;
  if (li_cmp_c[0] != 0 || li_cmp_c[1] != -1) return 220;
  uli_cmp_c = uli_cmp_a == uli_cmp_b;
  if (uli_cmp_c[0] != 0xffffffffffffffffULL || uli_cmp_c[1] != 0) return 221;
  uli_cmp_c = uli_cmp_a != uli_cmp_b;
  if (uli_cmp_c[0] != 0 || uli_cmp_c[1] != 0xffffffffffffffffULL) return 222;
  li_cmp_c = li_cmp_a < li_cmp_b;
  if (li_cmp_c[0] != 0 || li_cmp_c[1] != -1) return 223;
  uli_cmp_c = uli_cmp_a > uli_cmp_b;
  if (uli_cmp_c[0] != 0 || uli_cmp_c[1] != 0xffffffffffffffffULL) return 224;
  return 0;
}
