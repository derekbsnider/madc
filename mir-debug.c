/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* Generic GDB-JIT DWARF debug-object emitter -- see mir-debug.h. */

#include "mir-debug.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include(<elf.h>)
#include <elf.h>
#define MIR_DEBUG_HAVE_ELF 1
#endif
#endif

/* ===== growable byte buffer + LEB128 ===================================== */
typedef struct {
  unsigned char *p;
  size_t len, cap;
} dwbuf_t;

static int buf_reserve (dwbuf_t *b, size_t n) {
  if (b->len + n <= b->cap) return 0;
  size_t c = b->cap ? b->cap * 2 : 256;
  while (c < b->len + n) c *= 2;
  void *np = realloc (b->p, c);
  if (np == NULL) return -1;
  b->p = np;
  b->cap = c;
  return 0;
}
static void buf_bytes (dwbuf_t *b, const void *d, size_t n) {
  if (buf_reserve (b, n)) return;
  memcpy (b->p + b->len, d, n);
  b->len += n;
}
static void buf_u8 (dwbuf_t *b, uint8_t v) { buf_bytes (b, &v, 1); }
static void buf_u16 (dwbuf_t *b, uint16_t v) { buf_bytes (b, &v, 2); }
static void buf_u32 (dwbuf_t *b, uint32_t v) { buf_bytes (b, &v, 4); }
static void buf_u64 (dwbuf_t *b, uint64_t v) { buf_bytes (b, &v, 8); }
static void buf_str (dwbuf_t *b, const char *s) { buf_bytes (b, s ? s : "", (s ? strlen (s) : 0) + 1); }
static void buf_uleb (dwbuf_t *b, uint64_t v) {
  do {
    uint8_t x = v & 0x7f;
    v >>= 7;
    if (v) x |= 0x80;
    buf_u8 (b, x);
  } while (v);
}
static void buf_sleb (dwbuf_t *b, int64_t v) {
  for (;;) {
    uint8_t x = v & 0x7f;
    v >>= 7; /* arithmetic */
    int done = (v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40));
    if (!done) x |= 0x80;
    buf_u8 (b, x);
    if (done) break;
  }
}

/* ===== DWARF constants =================================================== */
enum {
  DW_TAG_array_type = 0x01,
  DW_TAG_enumeration_type = 0x04,
  DW_TAG_formal_parameter = 0x05,
  DW_TAG_member = 0x0d,
  DW_TAG_pointer_type = 0x0f,
  DW_TAG_compile_unit = 0x11,
  DW_TAG_structure_type = 0x13,
  DW_TAG_subroutine_type = 0x15,
  DW_TAG_typedef = 0x16,
  DW_TAG_union_type = 0x17,
  DW_TAG_unspecified_type = 0x3b,
  DW_TAG_subrange_type = 0x21,
  DW_TAG_base_type = 0x24,
  DW_TAG_subprogram = 0x2e,
  DW_TAG_variable = 0x34,
  DW_TAG_enumerator = 0x28,
  DW_CHILDREN_no = 0,
  DW_CHILDREN_yes = 1,
  DW_AT_location = 0x02,
  DW_AT_name = 0x03,
  DW_AT_byte_size = 0x0b,
  DW_AT_bit_size = 0x0d,
  DW_AT_stmt_list = 0x10,
  DW_AT_low_pc = 0x11,
  DW_AT_high_pc = 0x12,
  DW_AT_language = 0x13,
  DW_AT_comp_dir = 0x1b,
  DW_AT_const_value = 0x1c,
  DW_AT_producer = 0x25,
  DW_AT_count = 0x37,
  DW_AT_data_member_location = 0x38,
  DW_AT_data_bit_offset = 0x6b,
  DW_AT_encoding = 0x3e,
  DW_AT_external = 0x3f,
  DW_AT_frame_base = 0x40,
  DW_AT_type = 0x49,
  DW_FORM_addr = 0x01,
  DW_FORM_data2 = 0x05,
  DW_FORM_data1 = 0x0b,
  DW_FORM_sdata = 0x0d,
  DW_FORM_udata = 0x0f,
  DW_FORM_string = 0x08,
  DW_FORM_flag = 0x0c,
  DW_FORM_ref4 = 0x13,
  DW_FORM_sec_offset = 0x17,
  DW_FORM_exprloc = 0x18,
  DW_ATE_boolean = 0x02,
  DW_ATE_float = 0x04,
  DW_ATE_signed = 0x05,
  DW_ATE_signed_char = 0x06,
  DW_ATE_unsigned = 0x07,
  DW_ATE_unsigned_char = 0x08,
  DW_OP_deref = 0x06,
  DW_OP_plus_uconst = 0x23,
  DW_OP_fbreg = 0x91,
  DW_OP_reg6 = 0x56,  /* x86_64 rbp */
  DW_OP_reg29 = 0x6d, /* aarch64 x29 */
  DW_LANG_C99 = 0x0c,
  DW_LNS_copy = 1,
  DW_LNS_advance_pc = 2,
  DW_LNS_advance_line = 3,
  DW_LNS_set_file = 4,
  DW_LNS_set_prologue_end = 10,
  DW_LNE_end_sequence = 1,
  DW_LNE_set_address = 2,
};

/* abbrev codes */
enum {
  A_CU = 1, A_SUBPROG, A_BASE, A_PTR, A_VAR, A_PARAM, A_VOIDT,
  A_STRUCT, A_UNION, A_MEMBER, A_MEMBERBF, A_ARRAY, A_SUBRANGE, A_SUBRANGE0,
  A_ENUM, A_ENUMERATOR, A_SUBR, A_FPARAM, A_TYPEDEF,
};

#if defined(__aarch64__)
#define MIR_DEBUG_FP_OP DW_OP_reg29
#define MIR_DEBUG_EM EM_AARCH64
#elif defined(__x86_64__)
#define MIR_DEBUG_FP_OP DW_OP_reg6
#define MIR_DEBUG_EM EM_X86_64
#elif defined(__riscv) && __riscv_xlen == 64
#define MIR_DEBUG_FP_OP DW_OP_reg6 /* not validated; see frontend note */
#define MIR_DEBUG_EM EM_RISCV
#elif defined(__powerpc64__)
#define MIR_DEBUG_FP_OP DW_OP_reg6
#define MIR_DEBUG_EM EM_PPC64
#elif defined(__s390x__)
#define MIR_DEBUG_FP_OP DW_OP_reg6
#define MIR_DEBUG_EM EM_S390
#else
#define MIR_DEBUG_FP_OP DW_OP_reg6
#define MIR_DEBUG_EM 0
#endif

/* ===== builder state ===================================================== */
enum { DT_VOID, DT_BASE, DT_PTR, DT_ARRAY, DT_STRUCT, DT_UNION, DT_ENUM, DT_TYPEDEF, DT_FUNC };

typedef struct {
  int kind, enc;
  int64_t size, count;
  uint32_t ref;     /* ptr/array/typedef/func-return target */
  char *name;
  int first_member, last_member; /* members (struct/union), enumerators (enum), params (func) */
} dwtype_t;

typedef struct {
  char *name;
  uint32_t type;
  int64_t off;        /* member byte offset, or enumerator value */
  int bit_size, bit_off;
  int is_enumerator;  /* off is a value, type unused */
  int next;
} dwmember_t;

typedef struct {
  char *name;
  const void *addr;
  size_t size;
  MIR_line_map_t *line_map;
  size_t line_map_len;
  int first_var, last_var;
} dwfunc_t;

typedef struct {
  char *name;
  int is_param, deref_p;
  uint32_t type;
  int64_t fp_offset;
  uint64_t member_offset; /* added (DW_OP_plus_uconst) after the optional deref */
  int next;
} dwvar_t;

#define DEFVEC(T, field)                                                         \
  T *field;                                                                      \
  int n_##field, c_##field
struct MIR_debug {
  DEFVEC (char *, files);
  DEFVEC (dwtype_t, types);
  DEFVEC (dwmember_t, members);
  DEFVEC (dwfunc_t, funcs);
  DEFVEC (dwvar_t, vars);
  int cur_func;
};
#undef DEFVEC

#define VEC_PUSH(d, field, val)                                                  \
  do {                                                                           \
    if ((d)->n_##field == (d)->c_##field) {                                      \
      (d)->c_##field = (d)->c_##field ? (d)->c_##field * 2 : 16;                 \
      (d)->field = realloc ((d)->field, (size_t) (d)->c_##field * sizeof (*(d)->field)); \
    }                                                                            \
    (d)->field[(d)->n_##field++] = (val);                                        \
  } while (0)

static char *dw_strdup (const char *s) {
  if (s == NULL) return NULL;
  size_t n = strlen (s) + 1;
  char *r = malloc (n);
  if (r != NULL) memcpy (r, s, n);
  return r;
}

MIR_debug_t MIR_debug_init (void) {
  if (MIR_DEBUG_EM == 0) return NULL;
  MIR_debug_t d = calloc (1, sizeof (struct MIR_debug));
  if (d == NULL) return NULL;
  /* type 0 == void */
  dwtype_t v = {.kind = DT_VOID, .ref = 0, .count = -1, .first_member = -1, .last_member = -1,
                .name = dw_strdup ("void")};
  VEC_PUSH (d, types, v);
  return d;
}

void MIR_debug_destroy (MIR_debug_t d) {
  if (d == NULL) return;
  for (int i = 0; i < d->n_files; i++) free (d->files[i]);
  free (d->files);
  for (int i = 0; i < d->n_types; i++) free (d->types[i].name);
  free (d->types);
  for (int i = 0; i < d->n_members; i++) free (d->members[i].name);
  free (d->members);
  for (int i = 0; i < d->n_funcs; i++) {
    free (d->funcs[i].name);
    free (d->funcs[i].line_map);
  }
  free (d->funcs);
  for (int i = 0; i < d->n_vars; i++) free (d->vars[i].name);
  free (d->vars);
  free (d);
}

uint32_t MIR_debug_add_file (MIR_debug_t d, const char *path) {
  for (int i = 0; i < d->n_files; i++)
    if (strcmp (d->files[i], path) == 0) return (uint32_t) (i + 1);
  char *s = dw_strdup (path);
  VEC_PUSH (d, files, s);
  return (uint32_t) d->n_files;
}

static uint32_t new_type (MIR_debug_t d, int kind) {
  dwtype_t t = {.kind = kind, .ref = 0, .count = -1, .first_member = -1, .last_member = -1};
  VEC_PUSH (d, types, t);
  return (uint32_t) (d->n_types - 1);
}

MIR_debug_type_t MIR_debug_base_type (MIR_debug_t d, const char *name, MIR_debug_encoding_t enc,
                                      int byte_size) {
  static const int map[] = {0, DW_ATE_signed, DW_ATE_unsigned, DW_ATE_float, DW_ATE_boolean,
                            DW_ATE_signed_char, DW_ATE_unsigned_char};
  uint32_t i = new_type (d, DT_BASE);
  d->types[i].enc = (enc >= 1 && enc <= 6) ? map[enc] : DW_ATE_signed;
  d->types[i].size = byte_size;
  d->types[i].name = dw_strdup (name);
  return i;
}
MIR_debug_type_t MIR_debug_pointer_type (MIR_debug_t d, MIR_debug_type_t pointee) {
  uint32_t i = new_type (d, DT_PTR);
  d->types[i].ref = pointee;
  d->types[i].size = sizeof (void *);
  return i;
}
MIR_debug_type_t MIR_debug_array_type (MIR_debug_t d, MIR_debug_type_t elem, int64_t count) {
  uint32_t i = new_type (d, DT_ARRAY);
  d->types[i].ref = elem;
  d->types[i].count = count;
  return i;
}
MIR_debug_type_t MIR_debug_typedef_type (MIR_debug_t d, const char *name, MIR_debug_type_t ref) {
  uint32_t i = new_type (d, DT_TYPEDEF);
  d->types[i].ref = ref;
  d->types[i].name = dw_strdup (name);
  return i;
}
MIR_debug_type_t MIR_debug_struct_type (MIR_debug_t d, const char *name, int64_t byte_size,
                                        int is_union) {
  uint32_t i = new_type (d, is_union ? DT_UNION : DT_STRUCT);
  d->types[i].size = byte_size;
  d->types[i].name = dw_strdup (name);
  return i;
}
MIR_debug_type_t MIR_debug_enum_type (MIR_debug_t d, const char *name, int64_t byte_size) {
  uint32_t i = new_type (d, DT_ENUM);
  d->types[i].size = byte_size;
  d->types[i].name = dw_strdup (name);
  return i;
}
MIR_debug_type_t MIR_debug_func_type (MIR_debug_t d, MIR_debug_type_t ret_type) {
  uint32_t i = new_type (d, DT_FUNC);
  d->types[i].ref = ret_type;
  return i;
}

static void append_member (MIR_debug_t d, MIR_debug_type_t agg, dwmember_t m) {
  m.next = -1;
  VEC_PUSH (d, members, m);
  int idx = d->n_members - 1;
  if (d->types[agg].first_member < 0)
    d->types[agg].first_member = idx;
  else
    d->members[d->types[agg].last_member].next = idx;
  d->types[agg].last_member = idx;
}

void MIR_debug_add_member (MIR_debug_t d, MIR_debug_type_t agg, const char *name,
                           MIR_debug_type_t type, int64_t byte_offset) {
  dwmember_t m = {.name = dw_strdup (name), .type = type, .off = byte_offset};
  append_member (d, agg, m);
}
void MIR_debug_add_bitfield (MIR_debug_t d, MIR_debug_type_t agg, const char *name,
                             MIR_debug_type_t type, int64_t bit_offset, int bit_size) {
  dwmember_t m = {.name = dw_strdup (name), .type = type, .bit_off = (int) bit_offset,
                  .bit_size = bit_size};
  append_member (d, agg, m);
}
void MIR_debug_add_enumerator (MIR_debug_t d, MIR_debug_type_t en, const char *name, int64_t value) {
  dwmember_t m = {.name = dw_strdup (name), .off = value, .is_enumerator = 1};
  append_member (d, en, m);
}
void MIR_debug_add_param_type (MIR_debug_t d, MIR_debug_type_t fn, MIR_debug_type_t type) {
  dwmember_t m = {.type = type};
  append_member (d, fn, m);
}

void MIR_debug_add_func (MIR_debug_t d, const char *name, const void *addr, size_t size,
                         const MIR_line_map_t *line_map, size_t line_map_len) {
  MIR_line_map_t *lm = NULL;
  if (line_map != NULL && line_map_len != 0) {
    lm = malloc (line_map_len * sizeof (MIR_line_map_t));
    if (lm != NULL) memcpy (lm, line_map, line_map_len * sizeof (MIR_line_map_t));
  }
  dwfunc_t f = {.name = dw_strdup (name), .addr = addr, .size = size, .line_map = lm,
                .line_map_len = lm ? line_map_len : 0, .first_var = -1, .last_var = -1};
  VEC_PUSH (d, funcs, f);
  d->cur_func = d->n_funcs - 1;
}

void MIR_debug_add_var (MIR_debug_t d, const char *name, int is_param, MIR_debug_type_t type,
                        int64_t fp_offset, int deref_p, uint64_t member_offset) {
  if (d->n_funcs == 0) return;
  dwvar_t v = {.name = dw_strdup (name), .is_param = is_param, .type = type, .fp_offset = fp_offset,
               .member_offset = member_offset, .deref_p = deref_p, .next = -1};
  VEC_PUSH (d, vars, v);
  int idx = d->n_vars - 1;
  dwfunc_t *f = &d->funcs[d->cur_func];
  if (f->first_var < 0)
    f->first_var = idx;
  else
    d->vars[f->last_var].next = idx;
  f->last_var = idx;
}

/* ===== DWARF emission ==================================================== */

static void ab_hdr (dwbuf_t *b, int code, int tag, int children) {
  buf_uleb (b, code);
  buf_uleb (b, tag);
  buf_u8 (b, children);
}
static void ab_attr (dwbuf_t *b, int at, int form) {
  buf_uleb (b, at);
  buf_uleb (b, form);
}

static void emit_abbrev (dwbuf_t *b) {
  ab_hdr (b, A_CU, DW_TAG_compile_unit, DW_CHILDREN_yes);
  ab_attr (b, DW_AT_producer, DW_FORM_string);
  ab_attr (b, DW_AT_language, DW_FORM_data2);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_comp_dir, DW_FORM_string);
  ab_attr (b, DW_AT_low_pc, DW_FORM_addr);
  ab_attr (b, DW_AT_high_pc, DW_FORM_addr);
  ab_attr (b, DW_AT_stmt_list, DW_FORM_sec_offset);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_SUBPROG, DW_TAG_subprogram, DW_CHILDREN_yes);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_low_pc, DW_FORM_addr);
  ab_attr (b, DW_AT_high_pc, DW_FORM_addr);
  ab_attr (b, DW_AT_frame_base, DW_FORM_exprloc);
  ab_attr (b, DW_AT_external, DW_FORM_flag);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_BASE, DW_TAG_base_type, DW_CHILDREN_no);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_encoding, DW_FORM_data1);
  ab_attr (b, DW_AT_byte_size, DW_FORM_data1);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_PTR, DW_TAG_pointer_type, DW_CHILDREN_no);
  ab_attr (b, DW_AT_byte_size, DW_FORM_data1);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  buf_uleb (b, 0); buf_uleb (b, 0);

  for (int v = 0; v < 2; v++) {
    ab_hdr (b, v ? A_PARAM : A_VAR, v ? DW_TAG_formal_parameter : DW_TAG_variable, DW_CHILDREN_no);
    ab_attr (b, DW_AT_name, DW_FORM_string);
    ab_attr (b, DW_AT_type, DW_FORM_ref4);
    ab_attr (b, DW_AT_location, DW_FORM_exprloc);
    buf_uleb (b, 0); buf_uleb (b, 0);
  }

  ab_hdr (b, A_VOIDT, DW_TAG_unspecified_type, DW_CHILDREN_no);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  buf_uleb (b, 0); buf_uleb (b, 0);

  for (int u = 0; u < 2; u++) {
    ab_hdr (b, u ? A_UNION : A_STRUCT, u ? DW_TAG_union_type : DW_TAG_structure_type,
            DW_CHILDREN_yes);
    ab_attr (b, DW_AT_name, DW_FORM_string);
    ab_attr (b, DW_AT_byte_size, DW_FORM_udata);
    buf_uleb (b, 0); buf_uleb (b, 0);
  }

  ab_hdr (b, A_MEMBER, DW_TAG_member, DW_CHILDREN_no);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  ab_attr (b, DW_AT_data_member_location, DW_FORM_udata);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_MEMBERBF, DW_TAG_member, DW_CHILDREN_no);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  ab_attr (b, DW_AT_data_bit_offset, DW_FORM_udata);
  ab_attr (b, DW_AT_bit_size, DW_FORM_udata);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_ARRAY, DW_TAG_array_type, DW_CHILDREN_yes);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_SUBRANGE, DW_TAG_subrange_type, DW_CHILDREN_no);
  ab_attr (b, DW_AT_count, DW_FORM_udata);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_SUBRANGE0, DW_TAG_subrange_type, DW_CHILDREN_no);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_ENUM, DW_TAG_enumeration_type, DW_CHILDREN_yes);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_byte_size, DW_FORM_udata);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_ENUMERATOR, DW_TAG_enumerator, DW_CHILDREN_no);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_const_value, DW_FORM_sdata);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_SUBR, DW_TAG_subroutine_type, DW_CHILDREN_yes);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_FPARAM, DW_TAG_formal_parameter, DW_CHILDREN_no);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_TYPEDEF, DW_TAG_typedef, DW_CHILDREN_no);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  buf_uleb (b, 0); buf_uleb (b, 0);

  buf_uleb (b, 0); /* end of table */
}

typedef struct {
  size_t pos;
  uint32_t target;
} tfix_t;

static void emit_info (MIR_debug_t d, dwbuf_t *b, uint64_t text_base, uint64_t text_size,
                       const char *cu_name) {
  size_t unit_len_pos = b->len;
  buf_u32 (b, 0);
  size_t after_len = b->len;
  buf_u16 (b, 4);
  buf_u32 (b, 0);
  buf_u8 (b, 8);
  buf_uleb (b, A_CU);
  buf_str (b, "mir-debug");
  buf_u16 (b, DW_LANG_C99);
  buf_str (b, cu_name ? cu_name : "");
  buf_str (b, "");
  buf_u64 (b, text_base);
  buf_u64 (b, text_base + text_size);
  buf_u32 (b, 0); /* stmt_list */

  uint32_t *toff = d->n_types ? calloc ((size_t) d->n_types, sizeof (uint32_t)) : NULL;
  tfix_t *fix = NULL;
  int nfix = 0, cfix = 0;
#define REF(target_)                                                                          \
  do {                                                                                        \
    if (nfix == cfix) { cfix = cfix ? cfix * 2 : 64; fix = realloc (fix, (size_t) cfix * sizeof (tfix_t)); } \
    fix[nfix].pos = b->len; fix[nfix].target = (target_); nfix++; buf_u32 (b, 0);             \
  } while (0)

  for (int t = 0; t < d->n_types; t++) {
    dwtype_t *dt = &d->types[t];
    toff[t] = (uint32_t) (b->len - unit_len_pos);
    switch (dt->kind) {
    case DT_BASE:
      buf_uleb (b, A_BASE); buf_str (b, dt->name); buf_u8 (b, (uint8_t) dt->enc);
      buf_u8 (b, (uint8_t) dt->size);
      break;
    case DT_PTR:
      buf_uleb (b, A_PTR); buf_u8 (b, (uint8_t) (dt->size ? dt->size : sizeof (void *)));
      REF (dt->ref);
      break;
    case DT_TYPEDEF:
      buf_uleb (b, A_TYPEDEF); buf_str (b, dt->name); REF (dt->ref);
      break;
    case DT_STRUCT: case DT_UNION:
      buf_uleb (b, dt->kind == DT_UNION ? A_UNION : A_STRUCT);
      buf_str (b, dt->name);
      buf_uleb (b, (uint64_t) dt->size);
      for (int m = dt->first_member; m >= 0; m = d->members[m].next) {
        dwmember_t *dm = &d->members[m];
        if (dm->bit_size > 0) {
          buf_uleb (b, A_MEMBERBF); buf_str (b, dm->name); REF (dm->type);
          buf_uleb (b, (uint64_t) dm->bit_off); buf_uleb (b, (uint64_t) dm->bit_size);
        } else {
          buf_uleb (b, A_MEMBER); buf_str (b, dm->name); REF (dm->type);
          buf_uleb (b, (uint64_t) dm->off);
        }
      }
      buf_u8 (b, 0);
      break;
    case DT_ARRAY:
      buf_uleb (b, A_ARRAY); REF (dt->ref);
      if (dt->count >= 0) { buf_uleb (b, A_SUBRANGE); buf_uleb (b, (uint64_t) dt->count); }
      else buf_uleb (b, A_SUBRANGE0);
      buf_u8 (b, 0);
      break;
    case DT_ENUM:
      buf_uleb (b, A_ENUM); buf_str (b, dt->name); buf_uleb (b, (uint64_t) dt->size);
      for (int m = dt->first_member; m >= 0; m = d->members[m].next) {
        buf_uleb (b, A_ENUMERATOR); buf_str (b, d->members[m].name); buf_sleb (b, d->members[m].off);
      }
      buf_u8 (b, 0);
      break;
    case DT_FUNC:
      buf_uleb (b, A_SUBR); REF (dt->ref);
      for (int m = dt->first_member; m >= 0; m = d->members[m].next) {
        buf_uleb (b, A_FPARAM); REF (d->members[m].type);
      }
      buf_u8 (b, 0);
      break;
    default: /* DT_VOID */
      buf_uleb (b, A_VOIDT); buf_str (b, dt->name ? dt->name : "void");
      break;
    }
  }
  for (int f = 0; f < nfix; f++)
    if (toff != NULL) memcpy (b->p + fix[f].pos, &toff[fix[f].target], 4);
  free (fix);

  for (int i = 0; i < d->n_funcs; i++) {
    dwfunc_t *fn = &d->funcs[i];
    if (fn->addr == NULL) continue;
    buf_uleb (b, A_SUBPROG);
    buf_str (b, fn->name);
    buf_u64 (b, (uint64_t) (uintptr_t) fn->addr);
    buf_u64 (b, (uint64_t) (uintptr_t) fn->addr + fn->size);
    buf_u8 (b, 1); buf_u8 (b, MIR_DEBUG_FP_OP);
    buf_u8 (b, 1);
    for (int v = fn->first_var; v >= 0; v = d->vars[v].next) {
      dwvar_t *dv = &d->vars[v];
      buf_uleb (b, dv->is_param ? A_PARAM : A_VAR);
      buf_str (b, dv->name);
      buf_u32 (b, toff ? toff[dv->type] : 0);
      dwbuf_t e = {0};
      buf_u8 (&e, DW_OP_fbreg);
      buf_sleb (&e, dv->fp_offset);
      if (dv->deref_p) buf_u8 (&e, DW_OP_deref);
      if (dv->member_offset != 0) {
        buf_u8 (&e, DW_OP_plus_uconst);
        buf_uleb (&e, dv->member_offset);
      }
      buf_uleb (b, e.len);
      buf_bytes (b, e.p, e.len);
      free (e.p);
    }
    buf_u8 (b, 0);
  }
  buf_u8 (b, 0);
  free (toff);
  uint32_t unit_len = (uint32_t) (b->len - after_len);
  memcpy (b->p + unit_len_pos, &unit_len, 4);
#undef REF
}

static void emit_line (MIR_debug_t d, dwbuf_t *b) {
  size_t unit_len_pos = b->len;
  buf_u32 (b, 0);
  size_t after_len = b->len;
  buf_u16 (b, 4);
  size_t hdr_len_pos = b->len;
  buf_u32 (b, 0);
  size_t after_hdr_len = b->len;
  buf_u8 (b, 1);
  buf_u8 (b, 1);
  buf_u8 (b, 1);
  buf_u8 (b, (uint8_t) (int8_t) -5);
  buf_u8 (b, 14);
  buf_u8 (b, 13);
  static const uint8_t std_lens[12] = {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
  buf_bytes (b, std_lens, sizeof std_lens);
  buf_u8 (b, 0); /* include_directories */
  for (int i = 0; i < d->n_files; i++) {
    buf_str (b, d->files[i]);
    buf_uleb (b, 0); buf_uleb (b, 0); buf_uleb (b, 0);
  }
  buf_u8 (b, 0);
  uint32_t hdr_len = (uint32_t) (b->len - after_hdr_len);
  memcpy (b->p + hdr_len_pos, &hdr_len, 4);

  for (int i = 0; i < d->n_funcs; i++) {
    dwfunc_t *fn = &d->funcs[i];
    if (fn->addr == NULL || fn->line_map == NULL || fn->line_map_len == 0) continue;
    buf_u8 (b, 0); buf_uleb (b, 9); buf_u8 (b, DW_LNE_set_address);
    buf_u64 (b, (uint64_t) (uintptr_t) fn->addr);
    uint32_t cur_off = 0, cur_line = 1, cur_file = 1;
    /* entry-point row carrying the first statement's line + prologue_end */
    {
      MIR_line_map_t *e0 = &fn->line_map[0];
      uint32_t f0 = e0->file_id ? e0->file_id : 1;
      if (f0 != cur_file) { buf_u8 (b, DW_LNS_set_file); buf_uleb (b, f0); cur_file = f0; }
      buf_u8 (b, DW_LNS_advance_line); buf_sleb (b, (int64_t) e0->line - (int64_t) cur_line);
      cur_line = e0->line;
      buf_u8 (b, DW_LNS_copy);
    }
    int prologue_marked = 0;
    for (size_t j = 0; j < fn->line_map_len; j++) {
      MIR_line_map_t *e = &fn->line_map[j];
      uint32_t file = e->file_id ? e->file_id : 1;
      if (file != cur_file) { buf_u8 (b, DW_LNS_set_file); buf_uleb (b, file); cur_file = file; }
      if (e->line != cur_line) {
        buf_u8 (b, DW_LNS_advance_line); buf_sleb (b, (int64_t) e->line - (int64_t) cur_line);
        cur_line = e->line;
      }
      if (e->code_offset != cur_off) {
        buf_u8 (b, DW_LNS_advance_pc); buf_uleb (b, (uint64_t) (e->code_offset - cur_off));
        cur_off = e->code_offset;
      }
      if (!prologue_marked && e->code_offset != 0) { buf_u8 (b, DW_LNS_set_prologue_end); prologue_marked = 1; }
      buf_u8 (b, DW_LNS_copy);
    }
    if (fn->size > cur_off) { buf_u8 (b, DW_LNS_advance_pc); buf_uleb (b, (uint64_t) (fn->size - cur_off)); }
    buf_u8 (b, 0); buf_uleb (b, 1); buf_u8 (b, DW_LNE_end_sequence);
  }
  uint32_t unit_len = (uint32_t) (b->len - after_len);
  memcpy (b->p + unit_len_pos, &unit_len, 4);
}

#ifdef MIR_DEBUG_HAVE_ELF
/* One ELF section descriptor for the output object assembler below.  A named
   type (rather than an anonymous struct + typeof) so the file stays compilable
   by C frontends without the GNU typeof extension -- e.g. MIR's own c2m, which
   builds mir-debug.c as part of its self-bootstrap. */
typedef struct {
  const char *name;
  uint32_t type, link, info, align;
  uint64_t flags, addr, entsize;
  const dwbuf_t *body;
  uint64_t size;
} dwsec_t;

#define DWSEC_MAX 16

/* Assemble an ELF object from section descriptors S[0..ns-1] (S[0] must be
   the zeroed null section).  Appends the .shstrtab section itself, so the
   caller must leave one free slot (ns + 1 <= DWSEC_MAX).  On success returns
   0 and sets *buf (malloc'd, caller-owned) and *size.  This is the one ELF
   writer core -- it serves both the GDB-JIT debug object (MIR_debug_emit) and
   the AOT relocatable object (MIR_object_emit). */
static int elf_assemble (dwsec_t *S, int ns, uint16_t e_machine, void **buf, size_t *size) {
  dwbuf_t shstr = {0};
  uint32_t name_off[DWSEC_MAX];

  if (ns + 1 > DWSEC_MAX) return -1;
  buf_u8 (&shstr, 0);
  name_off[0] = 0;
  for (int i = 1; i < ns; i++) {
    name_off[i] = (uint32_t) shstr.len;
    buf_str (&shstr, S[i].name);
  }
  int i_shstr = ns;
  name_off[i_shstr] = (uint32_t) shstr.len;
  buf_str (&shstr, ".shstrtab");
  S[ns++] = (dwsec_t) {".shstrtab", SHT_STRTAB, 0, 0, 1, 0, 0, 0, &shstr, shstr.len};

  size_t off = sizeof (Elf64_Ehdr);
  uint64_t sec_off[DWSEC_MAX] = {0};
  for (int i = 1; i < ns; i++) {
    if (S[i].type == SHT_NOBITS) {
      sec_off[i] = off;
      continue;
    }
    size_t align = S[i].align > 8 ? S[i].align : 8;
    off = (off + align - 1) & ~(align - 1);
    sec_off[i] = off;
    off += S[i].size;
  }
  off = (off + 7) & ~(size_t) 7;
  size_t shoff = off, total = shoff + (size_t) ns * sizeof (Elf64_Shdr);

  unsigned char *p = calloc (1, total);
  if (p == NULL) {
    free (shstr.p);
    return -1;
  }
  Elf64_Ehdr *eh = (Elf64_Ehdr *) p;
  eh->e_ident[EI_MAG0] = ELFMAG0;
  eh->e_ident[EI_MAG1] = ELFMAG1;
  eh->e_ident[EI_MAG2] = ELFMAG2;
  eh->e_ident[EI_MAG3] = ELFMAG3;
  eh->e_ident[EI_CLASS] = ELFCLASS64;
  eh->e_ident[EI_DATA] = ELFDATA2LSB;
  eh->e_ident[EI_VERSION] = EV_CURRENT;
  eh->e_ident[EI_OSABI] = ELFOSABI_SYSV;
  eh->e_type = ET_REL;
  eh->e_machine = e_machine;
  eh->e_version = EV_CURRENT;
  eh->e_shoff = shoff;
  eh->e_ehsize = sizeof (Elf64_Ehdr);
  eh->e_shentsize = sizeof (Elf64_Shdr);
  eh->e_shnum = (Elf64_Half) ns;
  eh->e_shstrndx = (Elf64_Half) i_shstr;

  Elf64_Shdr *sh = (Elf64_Shdr *) (p + shoff);
  for (int i = 1; i < ns; i++) {
    if (S[i].type != SHT_NOBITS && S[i].body != NULL && S[i].size)
      memcpy (p + sec_off[i], S[i].body->p, S[i].size);
    sh[i].sh_name = name_off[i];
    sh[i].sh_type = S[i].type;
    sh[i].sh_flags = S[i].flags;
    sh[i].sh_addr = S[i].addr;
    sh[i].sh_offset = (S[i].type == SHT_NOBITS) ? 0 : sec_off[i];
    sh[i].sh_size = S[i].size;
    sh[i].sh_link = S[i].link;
    sh[i].sh_info = S[i].info;
    sh[i].sh_addralign = S[i].align;
    sh[i].sh_entsize = S[i].entsize;
  }
  free (shstr.p);
  *buf = p;
  *size = total;
  return 0;
}

/* ===== .debug_frame (CFI) ================================================ */
/* Authoritative unwind info for the JIT frames (x86-64 only for now).  gdb's
   heuristic prologue analysis does not recognize MIR's FP prologue
   (mov %rbp,-8(%rsp); lea -8(%rsp),%rbp -- no push), so without CFI a
   backtrace cannot leave frame 0 even though the in-memory FP chain is
   canonical (saved rbp at [rbp], return address at [rbp+8]).  Debug codegen
   (spill-all) forces keep_fp_p, so every function with a prologue gets the
   same fixed shape and needs only a template FDE: CFA=rsp+8 at entry, rbp
   saved at CFA-16 after insn 1 (+5), CFA=rbp+16 after insn 2 (+10).  A
   function whose entry bytes do not match the signature (a prologue-less
   leaf) keeps the CIE default row, which is exact for it.  The 1-2 insn
   epilogue window where the restored rbp mislabels the CFA is accepted
   debug-tier imprecision. */
enum {
  DW_CFA_nop = 0x00,
  DW_CFA_def_cfa = 0x0c, /* ULEB reg, ULEB offset */
  DW_CFA_advance_loc = 0x40, /* low 6 bits = code delta */
  DW_CFA_offset = 0x80, /* low 6 bits = reg; ULEB factored offset */
};

static void buf_patch_u32 (dwbuf_t *b, size_t pos, uint32_t v) { memcpy (b->p + pos, &v, 4); }

static void emit_frame (MIR_debug_t d, dwbuf_t *b) {
#if defined(__x86_64__)
  static const uint8_t fp_prologue[] = {0x48, 0x89, 0x6c, 0x24, 0xf8,  /* mov %rbp,-0x8(%rsp) */
                                        0x48, 0x8d, 0x6c, 0x24, 0xf8}; /* lea -0x8(%rsp),%rbp */
  /* CIE (DWARF32 .debug_frame v1): CFA = rsp+8, return address at CFA-8 */
  size_t cie_off = b->len, len_pos = b->len;
  buf_u32 (b, 0); /* length, backpatched */
  buf_u32 (b, 0xffffffff); /* CIE id */
  buf_u8 (b, 1); /* version */
  buf_u8 (b, 0); /* augmentation "" */
  buf_uleb (b, 1); /* code alignment factor */
  buf_sleb (b, -8); /* data alignment factor */
  buf_u8 (b, 16); /* return address register (rip) */
  buf_u8 (b, DW_CFA_def_cfa);
  buf_uleb (b, 7); /* rsp */
  buf_uleb (b, 8); /* CFA = rsp+8 */
  buf_u8 (b, DW_CFA_offset | 16);
  buf_uleb (b, 1); /* ra at CFA + 1*(-8) */
  while ((b->len - len_pos) % 8 != 0) buf_u8 (b, DW_CFA_nop);
  buf_patch_u32 (b, len_pos, (uint32_t) (b->len - len_pos - 4));

  for (int i = 0; i < d->n_funcs; i++) {
    dwfunc_t *fn = &d->funcs[i];
    if (fn->addr == NULL || fn->size == 0) continue;
    size_t fde_len_pos = b->len;
    buf_u32 (b, 0); /* length, backpatched */
    buf_u32 (b, (uint32_t) cie_off); /* CIE pointer (section offset) */
    buf_u64 (b, (uint64_t) (uintptr_t) fn->addr); /* initial location (absolute) */
    buf_u64 (b, (uint64_t) fn->size); /* address range */
    if (fn->size >= sizeof (fp_prologue)
        && memcmp (fn->addr, fp_prologue, sizeof (fp_prologue)) == 0) {
      buf_u8 (b, DW_CFA_advance_loc | 5);
      buf_u8 (b, DW_CFA_offset | 6); /* rbp saved... */
      buf_uleb (b, 2); /* ...at CFA + 2*(-8) */
      buf_u8 (b, DW_CFA_advance_loc | 5);
      buf_u8 (b, DW_CFA_def_cfa);
      buf_uleb (b, 6); /* rbp */
      buf_uleb (b, 16); /* CFA = rbp+16 */
    }
    while ((b->len - fde_len_pos) % 8 != 0) buf_u8 (b, DW_CFA_nop);
    buf_patch_u32 (b, fde_len_pos, (uint32_t) (b->len - fde_len_pos - 4));
  }
#else
  (void) d;
  (void) b;
#endif
}

int MIR_debug_emit (MIR_debug_t d, void **buf, size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  if (d == NULL || buf == NULL || size == NULL || MIR_DEBUG_EM == 0) return -1;

  uintptr_t lo = UINTPTR_MAX, hi = 0;
  for (int i = 0; i < d->n_funcs; i++) {
    if (d->funcs[i].addr == NULL) continue;
    uintptr_t a = (uintptr_t) d->funcs[i].addr, e = a + (d->funcs[i].size ? d->funcs[i].size : 1);
    if (a < lo) lo = a;
    if (e > hi) hi = e;
  }
  if (lo == UINTPTR_MAX) lo = hi = 0;
  uint64_t text_base = lo, text_size = hi > lo ? (uint64_t) (hi - lo) : 0;

  /* section bodies */
  dwbuf_t strtab = {0}, symtab = {0}, abbrev = {0}, info = {0}, line = {0}, frame = {0};
  buf_u8 (&strtab, 0);
  Elf64_Sym z = {0};
  buf_bytes (&symtab, &z, sizeof z);
  for (int i = 0; i < d->n_funcs; i++) {
    dwfunc_t *fn = &d->funcs[i];
    Elf64_Sym s = {0};
    s.st_name = (Elf64_Word) strtab.len;
    buf_str (&strtab, fn->name ? fn->name : "");
    s.st_info = ELF64_ST_INFO (STB_GLOBAL, STT_FUNC);
    s.st_other = STV_DEFAULT;
    if (fn->addr != NULL) {
      s.st_shndx = 1; /* .text */
      s.st_value = (Elf64_Addr) ((uintptr_t) fn->addr - text_base);
    } else {
      s.st_shndx = SHN_UNDEF;
    }
    s.st_size = (Elf64_Xword) fn->size;
    buf_bytes (&symtab, &s, sizeof s);
  }
  const char *cu_name = d->n_files ? d->files[0] : "";
  emit_abbrev (&abbrev);
  emit_info (d, &info, text_base, text_size, cu_name);
  emit_line (d, &line);
  emit_frame (d, &frame);

  dwsec_t S[DWSEC_MAX];
  int ns = 0;
  S[ns++] = (dwsec_t){0};
  S[ns++] = (dwsec_t){".text", SHT_NOBITS, 0, 0, 16, SHF_ALLOC | SHF_EXECINSTR, text_base, 0,
                      NULL, text_size};
  int i_symtab = ns;
  S[ns++] = (dwsec_t){".symtab", SHT_SYMTAB, 0, 1, 8, 0, 0, sizeof (Elf64_Sym), &symtab,
                      symtab.len};
  int i_strtab = ns;
  S[ns++] = (dwsec_t){".strtab", SHT_STRTAB, 0, 0, 1, 0, 0, 0, &strtab, strtab.len};
  S[i_symtab].link = (uint32_t) i_strtab;
  S[ns++] = (dwsec_t){".debug_abbrev", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, &abbrev, abbrev.len};
  S[ns++] = (dwsec_t){".debug_info", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, &info, info.len};
  S[ns++] = (dwsec_t){".debug_line", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, &line, line.len};
  if (frame.len != 0)
    S[ns++] = (dwsec_t){".debug_frame", SHT_PROGBITS, 0, 0, 8, 0, 0, 0, &frame, frame.len};
  int rc = elf_assemble (S, ns, MIR_DEBUG_EM, buf, size);
  free (strtab.p); free (symtab.p); free (abbrev.p); free (info.p); free (line.p); free (frame.p);
  return rc;
}

/* ===== AOT relocatable-object builder (MIR_object_*) ===================== */
/* The second consumer of the ELF core above (see mir-debug.h).  Everything is
   section-relative and unresolved; the system linker applies the .rela.*
   entries.  Fixed section-header indexes: 1 .text, 2 .data, 3 .bss (then
   .symtab/.strtab and any .rela.* / .shstrtab). */

typedef struct {
  char *name; /* copied; NULL for section symbols and anonymous items */
  int sec;    /* MIR_OBJ_SEC_* or MIR_OBJ_SEC_UNDEF */
  uint64_t value, size;
  unsigned char func_p, local_p, weak_p, section_p, defined_p;
} objsym_t;

typedef struct {
  int sec; /* MIR_OBJ_SEC_TEXT or MIR_OBJ_SEC_DATA */
  uint64_t offset;
  int sym;
  int64_t addend;
  int kind;
} objreloc_t;

struct MIR_object {
  dwbuf_t text, data;
  uint64_t bss_size;
  size_t data_align, bss_align;
  objsym_t *syms;
  size_t n_syms, cap_syms;
  objreloc_t *rels;
  size_t n_rels, cap_rels;
  int sec_sym[3]; /* section-symbol ids for TEXT/DATA/BSS; -1 until created */
};

static void buf_zeros (dwbuf_t *b, size_t n) {
  if (buf_reserve (b, n)) return;
  memset (b->p + b->len, 0, n);
  b->len += n;
}

static char *obj_strdup (const char *s) {
  size_t n = strlen (s) + 1;
  char *p = malloc (n);
  if (p != NULL) memcpy (p, s, n);
  return p;
}

MIR_object_t MIR_object_create (void) {
  if (MIR_DEBUG_EM != EM_X86_64) return NULL; /* x86-64 first: the reloc mapping */
  MIR_object_t obj = calloc (1, sizeof (struct MIR_object));
  if (obj == NULL) return NULL;
  obj->data_align = obj->bss_align = 1;
  obj->sec_sym[0] = obj->sec_sym[1] = obj->sec_sym[2] = -1;
  return obj;
}

void MIR_object_destroy (MIR_object_t obj) {
  if (obj == NULL) return;
  for (size_t i = 0; i < obj->n_syms; i++) free (obj->syms[i].name);
  free (obj->syms);
  free (obj->rels);
  free (obj->text.p);
  free (obj->data.p);
  free (obj);
}

size_t MIR_object_text_append (MIR_object_t obj, const void *bytes, size_t len) {
  while (obj->text.len % 16 != 0) buf_u8 (&obj->text, 0);
  size_t off = obj->text.len;
  buf_bytes (&obj->text, bytes, len);
  return off;
}

size_t MIR_object_data_append (MIR_object_t obj, const void *bytes, size_t len, size_t align) {
  if (align > obj->data_align) obj->data_align = align;
  if (align > 1)
    while (obj->data.len % align != 0) buf_u8 (&obj->data, 0);
  size_t off = obj->data.len;
  if (bytes == NULL)
    buf_zeros (&obj->data, len);
  else
    buf_bytes (&obj->data, bytes, len);
  return off;
}

size_t MIR_object_bss_reserve (MIR_object_t obj, size_t len, size_t align) {
  if (align > obj->bss_align) obj->bss_align = align;
  if (align > 1) obj->bss_size = (obj->bss_size + align - 1) & ~((uint64_t) align - 1);
  size_t off = (size_t) obj->bss_size;
  obj->bss_size += len;
  return off;
}

int MIR_object_add_symbol (MIR_object_t obj, const char *name, int sec, uint64_t value,
                           uint64_t size, int func_p, int local_p, int weak_p) {
  if (obj->n_syms >= obj->cap_syms) {
    size_t c = obj->cap_syms ? obj->cap_syms * 2 : 64;
    void *np = realloc (obj->syms, c * sizeof (objsym_t));
    if (np == NULL) return -1;
    obj->syms = np;
    obj->cap_syms = c;
  }
  objsym_t *s = &obj->syms[obj->n_syms];
  memset (s, 0, sizeof *s);
  if (name != NULL && (s->name = obj_strdup (name)) == NULL) return -1;
  s->sec = sec;
  s->value = value;
  s->size = size;
  s->func_p = func_p != 0;
  s->local_p = local_p != 0;
  s->weak_p = weak_p != 0;
  s->defined_p = sec != MIR_OBJ_SEC_UNDEF;
  return (int) obj->n_syms++;
}

void MIR_object_symbol_define (MIR_object_t obj, int sym_id, int sec, uint64_t value,
                               uint64_t size, int func_p, int local_p, int weak_p) {
  if (sym_id < 0 || (size_t) sym_id >= obj->n_syms) return;
  objsym_t *s = &obj->syms[sym_id];
  s->sec = sec;
  s->value = value;
  s->size = size;
  s->func_p = func_p != 0;
  s->local_p = local_p != 0;
  s->weak_p = weak_p != 0;
  s->defined_p = sec != MIR_OBJ_SEC_UNDEF;
}

int MIR_object_section_symbol (MIR_object_t obj, int sec) {
  if (sec < MIR_OBJ_SEC_TEXT || sec > MIR_OBJ_SEC_BSS) return -1;
  if (obj->sec_sym[sec] < 0) {
    int id = MIR_object_add_symbol (obj, NULL, sec, 0, 0, 0, 1, 0);
    if (id >= 0) obj->syms[id].section_p = 1;
    obj->sec_sym[sec] = id;
  }
  return obj->sec_sym[sec];
}

int MIR_object_symbol_defined_p (MIR_object_t obj, int sym_id) {
  if (sym_id < 0 || (size_t) sym_id >= obj->n_syms) return 0;
  return obj->syms[sym_id].defined_p;
}

void MIR_object_add_reloc (MIR_object_t obj, int sec, uint64_t offset, int sym_id, int64_t addend,
                           int kind) {
  if (obj->n_rels >= obj->cap_rels) {
    size_t c = obj->cap_rels ? obj->cap_rels * 2 : 64;
    void *np = realloc (obj->rels, c * sizeof (objreloc_t));
    if (np == NULL) return;
    obj->rels = np;
    obj->cap_rels = c;
  }
  obj->rels[obj->n_rels++] = (objreloc_t) {sec, offset, sym_id, addend, kind};
}

int MIR_object_emit (MIR_object_t obj, void **buf, size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  if (obj == NULL || buf == NULL || size == NULL || MIR_DEBUG_EM != EM_X86_64) return -1;

  /* Final symtab order: null, locals (incl. section symbols), then
     globals/weak.  Relocations hold stable ids; map them here. */
  size_t n = obj->n_syms;
  uint32_t *final_idx = calloc (n ? n : 1, sizeof (uint32_t));
  if (final_idx == NULL) return -1;
  dwbuf_t strtab = {0}, symtab = {0}, rela_text = {0}, rela_data = {0};
  buf_u8 (&strtab, 0);
  Elf64_Sym z = {0};
  buf_bytes (&symtab, &z, sizeof z);
  static const int sec_shndx[3] = {1, 2, 3};
  uint32_t next = 1, n_locals = 0;
  for (int pass = 0; pass < 2; pass++)
    for (size_t i = 0; i < n; i++) {
      objsym_t *s = &obj->syms[i];
      int local = s->local_p || s->section_p;
      if ((pass == 0) != (local != 0)) continue;
      Elf64_Sym es = {0};
      if (s->name != NULL) {
        es.st_name = (Elf64_Word) strtab.len;
        buf_str (&strtab, s->name);
      }
      unsigned char bind = local ? STB_LOCAL : s->weak_p ? STB_WEAK : STB_GLOBAL;
      unsigned char type = s->section_p ? STT_SECTION
                           : !s->defined_p ? STT_NOTYPE
                           : s->func_p ? STT_FUNC
                                       : STT_OBJECT;
      es.st_info = ELF64_ST_INFO (bind, type);
      es.st_other = STV_DEFAULT;
      es.st_shndx = s->defined_p ? (Elf64_Section) sec_shndx[s->sec] : SHN_UNDEF;
      es.st_value = s->defined_p ? s->value : 0;
      es.st_size = s->size;
      buf_bytes (&symtab, &es, sizeof es);
      if (pass == 0) n_locals++;
      final_idx[i] = next++;
    }

  int bad_p = 0;
  for (size_t i = 0; i < obj->n_rels; i++) {
    objreloc_t *r = &obj->rels[i];
    if (r->kind != MIR_OBJ_RELOC_ABS64 || r->sym < 0 || (size_t) r->sym >= n
        || (r->sec != MIR_OBJ_SEC_TEXT && r->sec != MIR_OBJ_SEC_DATA)) {
      bad_p = 1;
      break;
    }
    Elf64_Rela er;
    er.r_offset = r->offset;
    er.r_info = ELF64_R_INFO ((uint64_t) final_idx[r->sym], R_X86_64_64);
    er.r_addend = r->addend;
    buf_bytes (r->sec == MIR_OBJ_SEC_TEXT ? &rela_text : &rela_data, &er, sizeof er);
  }

  int rc = -1;
  if (!bad_p) {
    dwsec_t S[DWSEC_MAX];
    int ns = 0;
    S[ns++] = (dwsec_t) {0};
    S[ns++] = (dwsec_t) {".text", SHT_PROGBITS, 0, 0, 16, SHF_ALLOC | SHF_EXECINSTR, 0, 0,
                         &obj->text, obj->text.len};
    S[ns++] = (dwsec_t) {".data", SHT_PROGBITS, 0, 0,
                         (uint32_t) (obj->data_align > 8 ? obj->data_align : 8),
                         SHF_ALLOC | SHF_WRITE, 0, 0, &obj->data, obj->data.len};
    S[ns++] = (dwsec_t) {".bss", SHT_NOBITS, 0, 0,
                         (uint32_t) (obj->bss_align > 8 ? obj->bss_align : 8),
                         SHF_ALLOC | SHF_WRITE, 0, 0, NULL, obj->bss_size};
    int i_symtab = ns;
    S[ns++] = (dwsec_t) {".symtab", SHT_SYMTAB, 0, n_locals + 1, 8, 0, 0, sizeof (Elf64_Sym),
                         &symtab, symtab.len};
    int i_strtab = ns;
    S[ns++] = (dwsec_t) {".strtab", SHT_STRTAB, 0, 0, 1, 0, 0, 0, &strtab, strtab.len};
    S[i_symtab].link = (uint32_t) i_strtab;
    if (rela_text.len != 0)
      S[ns++] = (dwsec_t) {".rela.text", SHT_RELA, (uint32_t) i_symtab, 1, 8, SHF_INFO_LINK, 0,
                           sizeof (Elf64_Rela), &rela_text, rela_text.len};
    if (rela_data.len != 0)
      S[ns++] = (dwsec_t) {".rela.data", SHT_RELA, (uint32_t) i_symtab, 2, 8, SHF_INFO_LINK, 0,
                           sizeof (Elf64_Rela), &rela_data, rela_data.len};
    /* non-executable stack marker (its absence makes ld assume an executable
       stack and warn) */
    S[ns++] = (dwsec_t) {".note.GNU-stack", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, NULL, 0};
    rc = elf_assemble (S, ns, EM_X86_64, buf, size);
  }
  free (final_idx);
  free (strtab.p);
  free (symtab.p);
  free (rela_text.p);
  free (rela_data.p);
  return rc;
}

/* ===== AOT executable emitter (ET_EXEC/ET_DYN, x86-64 Linux) ============= */
/* The third consumer of the object builder: a complete dynamic executable
   with no external toolchain.  This lays out segments/vaddrs by hand -- a
   different concern from elf_assemble's link-view section table (ET_REL,
   no program headers), so it is a sibling stage over the SAME builder data,
   not a parallel builder.  Model (from madc-master's proven in-house writer,
   simplified by MIR's exact reloc ledgers):
     - fixed load base 0x400000, identity file-offset<->vaddr mapping;
     - two PT_LOADs (R+X: headers/interp/hash/dynsym/dynstr/rela.dyn/text;
       R+W: data/.dynamic/bss tail) + PT_INTERP + PT_DYNAMIC;
     - internal relocations resolve at emit; imports become eager
       R_X86_64_64 slot relocations in .rela.dyn (no PLT/GOT: MIR calls
       already go through address slots) => DT_TEXTREL until the PIC rung;
     - synthesized _start: SysV stack -> __libc_start_main (entry, argc,
       argv, init=0, fini=0, rtld_fini) through its own reloc slot.
   shared_p flips the same layout to an ET_DYN shared object: base 0 (the
   loader picks the bias, so .dynamic/.rela.dyn/.dynsym values stay link-time
   vaddrs the loader rebases), no PT_INTERP/_start/entry, defined globals
   exported in .dynsym, and EVERY relocation kept dynamic -- internal targets
   as R_X86_64_RELATIVE (bias + link vaddr; -Bsymbolic semantics), imports as
   R_X86_64_64.  DT_INIT (params->init, when defined) covers load-time
   initializers for dlopen'd modules that have no main to call them. */

#define OBJX_BASE_ADDR 0x400000ull
#define OBJX_PAGE 0x1000ull

/* _start: xor ebp; mov rdx->r9; pop rsi (argc); mov rsp->rdx (argv);
   align rsp; push rax; push rsp; r8=0 (fini); ecx=0 (init);
   mov $entry,%edi (imm32 @0x15); call *disp32(%rip) (@0x19, disp @0x1b)
   through the 8-byte __libc_start_main slot right after the stub; hlt. */
static const uint8_t objx_start_stub[]
  = {0x31, 0xed, 0x49, 0x89, 0xd1, 0x5e, 0x48, 0x89, 0xe2, 0x48, 0x83,
     0xe4, 0xf0, 0x50, 0x54, 0x45, 0x31, 0xc0, 0x31, 0xc9, 0xbf, 0x00,
     0x00, 0x00, 0x00, 0xff, 0x15, 0x00, 0x00, 0x00, 0x00, 0xf4};
#define OBJX_STUB_ENTRY_IMM 21   /* imm32 of mov $entry,%edi */
#define OBJX_STUB_CALL_DISP 27   /* disp32 of call *(%rip) */
#define OBJX_STUB_SIZE 32
#define OBJX_LSM_SLOT OBJX_STUB_SIZE /* 8-byte __libc_start_main slot */
/* stub + slot padded to 16 so the captured text keeps the 16-byte alignment
   it was generated under (SSE pool constants: movaps/xorpd fault otherwise) */
#define OBJX_TEXT_BIAS 48

static uint32_t objx_elf_hash (const char *name) {
  uint32_t h = 0, g;
  for (; *name; ++name) {
    h = (h << 4) + (uint8_t) *name;
    g = h & 0xf0000000;
    if (g) h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

int MIR_object_emit_executable (MIR_object_t obj, const MIR_object_exec_params *params,
                                void **buf, size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  if (obj == NULL || params == NULL || buf == NULL || size == NULL || MIR_DEBUG_EM != EM_X86_64)
    return -1;
  int shared_p = params->shared_p != 0;
  uint64_t base = shared_p ? 0 : OBJX_BASE_ADDR;
  const char *interp = params->interp != NULL ? params->interp : "/lib64/ld-linux-x86-64.so.2";
  const char *entry_nm = params->entry != NULL ? params->entry : "main";
  static const Elf64_Section objx_shndx[3] = {6, 7, 9}; /* text/data/bss shdr indexes */

  /* the entry symbol must be a defined text function (executables only) */
  size_t n = obj->n_syms;
  size_t entry_i = n;
  if (!shared_p) {
    for (size_t i = 0; i < n; i++)
      if (obj->syms[i].name != NULL && obj->syms[i].defined_p
          && obj->syms[i].sec == MIR_OBJ_SEC_TEXT && strcmp (obj->syms[i].name, entry_nm) == 0) {
        entry_i = i;
        break;
      }
    if (entry_i == n) return -1;
  }
  /* optional DT_INIT symbol; a module without one is valid -- omit the tag */
  size_t init_i = n;
  if (params->init != NULL)
    for (size_t i = 0; i < n; i++)
      if (obj->syms[i].name != NULL && obj->syms[i].defined_p
          && obj->syms[i].sec == MIR_OBJ_SEC_TEXT && strcmp (obj->syms[i].name, params->init) == 0) {
        init_i = i;
        break;
      }

  /* dynsym = every undefined (imported) symbol, plus __libc_start_main for
     the _start slot if the module does not already import it (executables);
     shared objects additionally export their defined globals (st_value gets
     the resolved link vaddr patched in after layout) */
  uint32_t *dyn_idx = calloc (n ? n : 1, sizeof (uint32_t)); /* 0 = not imported */
  if (dyn_idx == NULL) return -1;
  dwbuf_t dynstr = {0}, dynsym = {0}, hash = {0}, symtab = {0}, strtab = {0}, shstr = {0};
  int rc = -1;
  unsigned char *p = NULL;
  uint32_t *lib_name_off = NULL;
  buf_u8 (&dynstr, 0);
  Elf64_Sym z = {0};
  buf_bytes (&dynsym, &z, sizeof z);
  lib_name_off = calloc (params->n_needed ? params->n_needed : 1, sizeof (uint32_t));
  if (lib_name_off == NULL) goto done;
  for (size_t i = 0; i < params->n_needed; i++) {
    lib_name_off[i] = (uint32_t) dynstr.len;
    buf_str (&dynstr, params->needed[i]);
  }
  uint32_t runpath_off = 0;
  if (params->runpath != NULL) {
    runpath_off = (uint32_t) dynstr.len;
    buf_str (&dynstr, params->runpath);
  }
  uint32_t ndyn = 1, lsm_idx = 0;
  const char **dyn_names = calloc (n + 1, sizeof (char *));
  if (dyn_names == NULL) goto done;
  for (size_t i = 0; i < n; i++) {
    objsym_t *s = &obj->syms[i];
    if (s->name == NULL) continue;
    if (s->defined_p && (!shared_p || s->local_p || s->section_p)) continue;
    Elf64_Sym es = {0};
    es.st_name = (Elf64_Word) dynstr.len;
    buf_str (&dynstr, s->name);
    if (s->defined_p) { /* shared-object export; st_value patched after layout */
      es.st_info = ELF64_ST_INFO (s->weak_p ? STB_WEAK : STB_GLOBAL,
                                  s->func_p ? STT_FUNC : STT_OBJECT);
      es.st_shndx = objx_shndx[s->sec];
      es.st_size = s->size;
    } else {
      es.st_info = ELF64_ST_INFO (STB_GLOBAL, s->func_p ? STT_FUNC : STT_NOTYPE);
    }
    buf_bytes (&dynsym, &es, sizeof es);
    dyn_names[ndyn] = s->name;
    dyn_idx[i] = ndyn++;
    if (!shared_p && strcmp (s->name, "__libc_start_main") == 0) lsm_idx = dyn_idx[i];
  }
  if (!shared_p && lsm_idx == 0) {
    Elf64_Sym es = {0};
    es.st_name = (Elf64_Word) dynstr.len;
    buf_str (&dynstr, "__libc_start_main");
    es.st_info = ELF64_ST_INFO (STB_GLOBAL, STT_FUNC);
    buf_bytes (&dynsym, &es, sizeof es);
    dyn_names[ndyn] = "__libc_start_main";
    lsm_idx = ndyn++;
  }

  { /* SysV .hash over the dynsyms */
    uint32_t nbucket = ndyn, nchain = ndyn;
    uint32_t *buckets = calloc (nbucket, sizeof (uint32_t));
    uint32_t *chains = calloc (nchain, sizeof (uint32_t));
    if (buckets == NULL || chains == NULL) {
      free (buckets);
      free (chains);
      free ((void *) dyn_names);
      goto done;
    }
    for (uint32_t i = 1; i < ndyn; i++) {
      uint32_t h = objx_elf_hash (dyn_names[i]) % nbucket;
      chains[i] = buckets[h];
      buckets[h] = i;
    }
    buf_u32 (&hash, nbucket);
    buf_u32 (&hash, nchain);
    for (uint32_t i = 0; i < nbucket; i++) buf_u32 (&hash, buckets[i]);
    for (uint32_t i = 0; i < nchain; i++) buf_u32 (&hash, chains[i]);
    free (buckets);
    free (chains);
  }
  free ((void *) dyn_names);

  /* executables: import relocations survive to .rela.dyn, internal ones
     resolve below; shared objects: the load bias is unknown, so EVERY
     relocation stays dynamic */
  size_t n_dyn_rel = shared_p ? 0 : 1; /* the __libc_start_main slot */
  for (size_t i = 0; i < obj->n_rels; i++) {
    objreloc_t *r = &obj->rels[i];
    if (r->kind != MIR_OBJ_RELOC_ABS64 || r->sym < 0 || (size_t) r->sym >= n) goto done;
    if (shared_p || !obj->syms[r->sym].defined_p) n_dyn_rel++;
  }

  /* ---- layout: identity file-offset<->vaddr mapping from OBJX_BASE_ADDR */
#define OBJX_ALIGN(v, a) (((v) + (uint64_t) (a) -1) & ~((uint64_t) (a) -1))
  uint64_t off = sizeof (Elf64_Ehdr);
  uint64_t phdr_off = off;
  const int n_phdrs = shared_p ? 3 : 4; /* shared objects carry no PT_INTERP */
  off += (uint64_t) n_phdrs * sizeof (Elf64_Phdr);
  uint64_t interp_off = off, interp_size = shared_p ? 0 : strlen (interp) + 1;
  off = OBJX_ALIGN (interp_off + interp_size, 8);
  uint64_t hash_off = off;
  off = OBJX_ALIGN (hash_off + hash.len, 8);
  uint64_t dynsym_off = off;
  off = OBJX_ALIGN (dynsym_off + dynsym.len, 1);
  uint64_t dynstr_off = off;
  off = OBJX_ALIGN (dynstr_off + dynstr.len, 8);
  uint64_t rela_off = off, rela_size = (uint64_t) n_dyn_rel * sizeof (Elf64_Rela);
  off = OBJX_ALIGN (rela_off + rela_size, 16);
  /* no _start stub/slot in a .so; text_off's 16-alignment already preserves
     the captured code's SSE-constant alignment on its own */
  uint64_t text_bias = shared_p ? 0 : OBJX_TEXT_BIAS;
  uint64_t text_off = off, text_size = text_bias + obj->text.len;
  off = OBJX_ALIGN (text_off + text_size, OBJX_PAGE);
  uint64_t rx_filesz = text_off + text_size; /* the R+X segment: file start..text end */
  uint64_t rw_off = off;                     /* page-aligned R+W segment start */
  size_t data_align = obj->data_align > 8 ? obj->data_align : 8;
  if (data_align > OBJX_PAGE) goto done; /* congruence bound; nothing emits >4K aligns */
  uint64_t data_off = rw_off;
  off = OBJX_ALIGN (data_off + obj->data.len, 8);
  uint64_t dynamic_off = off;
  /* NEEDED*n [+RUNPATH] [+INIT] + STRTAB/STRSZ/SYMTAB/SYMENT/HASH
     + RELA/RELASZ/RELAENT + TEXTREL (address slots live in text) + NULL */
  uint64_t n_dyntags
    = params->n_needed + (params->runpath != NULL ? 1 : 0) + (init_i < n ? 1 : 0) + 9 + 1;
  uint64_t dynamic_size = n_dyntags * sizeof (Elf64_Dyn);
  off = dynamic_off + dynamic_size;
  uint64_t rw_filesz = off - rw_off;
  size_t bss_align = obj->bss_align > 8 ? obj->bss_align : 8;
  uint64_t bss_vaddr = OBJX_ALIGN (base + off, bss_align);
  uint64_t rw_memsz = bss_vaddr + obj->bss_size - (base + rw_off);
  uint64_t text_vaddr = base + text_off;
  uint64_t data_vaddr = base + data_off;
  uint64_t code_vaddr = text_vaddr + text_bias; /* the builder's .text offset 0 */

  /* section vaddr for a defined symbol/section id */
#define OBJX_SEC_VADDR(sec) \
  ((sec) == MIR_OBJ_SEC_TEXT ? code_vaddr : (sec) == MIR_OBJ_SEC_DATA ? data_vaddr : bss_vaddr)

  /* ---- .symtab / .strtab (debug view: resolved link-time vaddrs) */
  buf_u8 (&strtab, 0);
  buf_bytes (&symtab, &z, sizeof z);
  uint32_t n_locals = 0, next = 1;
  for (int pass = 0; pass < 2; pass++)
    for (size_t i = 0; i < n; i++) {
      objsym_t *s = &obj->syms[i];
      int local = s->local_p || s->section_p;
      if ((pass == 0) != (local != 0)) continue;
      Elf64_Sym es = {0};
      if (s->name != NULL) {
        es.st_name = (Elf64_Word) strtab.len;
        buf_str (&strtab, s->name);
      }
      unsigned char bind = local ? STB_LOCAL : s->weak_p ? STB_WEAK : STB_GLOBAL;
      unsigned char type = s->section_p ? STT_SECTION
                           : !s->defined_p ? STT_NOTYPE
                           : s->func_p ? STT_FUNC
                                       : STT_OBJECT;
      es.st_info = ELF64_ST_INFO (bind, type);
      es.st_shndx = s->defined_p ? objx_shndx[s->sec] : SHN_UNDEF;
      es.st_value = s->defined_p ? OBJX_SEC_VADDR (s->sec) + s->value : 0;
      es.st_size = s->size;
      buf_bytes (&symtab, &es, sizeof es);
      if (pass == 0) n_locals++;
      next++;
    }
  (void) next;

  /* ---- assemble the image */
  const int n_secs = 13; /* see shdr table below */
  uint32_t shname[13];
  static const char *objx_sec_names[13]
    = {"",      ".interp", ".hash",    ".dynsym", ".dynstr", ".rela.dyn", ".text",
       ".data", ".dynamic", ".bss",    ".symtab", ".strtab", ".shstrtab"};
  buf_u8 (&shstr, 0);
  shname[0] = 0;
  for (int i = 1; i < n_secs; i++) {
    shname[i] = (uint32_t) shstr.len;
    buf_str (&shstr, objx_sec_names[i]);
  }
  uint64_t symtab_off = OBJX_ALIGN (off, 8);
  uint64_t strtab_off = symtab_off + symtab.len;
  uint64_t shstr_off = strtab_off + strtab.len;
  uint64_t shoff = OBJX_ALIGN (shstr_off + shstr.len, 8);
  uint64_t total = shoff + (uint64_t) n_secs * sizeof (Elf64_Shdr);

  p = calloc (1, (size_t) total);
  if (p == NULL) goto done;
  memcpy (p + hash_off, hash.p, hash.len);
  memcpy (p + dynsym_off, dynsym.p, dynsym.len);
  memcpy (p + dynstr_off, dynstr.p, dynstr.len);
  if (shared_p) { /* exported definitions: patch the resolved link-time vaddrs */
    Elf64_Sym *ds = (Elf64_Sym *) (p + dynsym_off);
    for (size_t i = 0; i < n; i++)
      if (dyn_idx[i] != 0 && obj->syms[i].defined_p)
        ds[dyn_idx[i]].st_value = OBJX_SEC_VADDR (obj->syms[i].sec) + obj->syms[i].value;
  } else {
    memcpy (p + interp_off, interp, interp_size);
    memcpy (p + text_off, objx_start_stub, OBJX_STUB_SIZE);
    uint64_t entry_vaddr = code_vaddr + obj->syms[entry_i].value;
    uint32_t imm = (uint32_t) entry_vaddr; /* base 0x400000: fits imm32 */
    memcpy (p + text_off + OBJX_STUB_ENTRY_IMM, &imm, 4);
    /* call *disp32(%rip): rip = stub end (+31); slot at +32 */
    uint32_t disp = (uint32_t) (OBJX_LSM_SLOT - (OBJX_STUB_CALL_DISP + 4));
    memcpy (p + text_off + OBJX_STUB_CALL_DISP, &disp, 4);
  }
  if (obj->text.len != 0) memcpy (p + text_off + text_bias, obj->text.p, obj->text.len);
  if (obj->data.len != 0) memcpy (p + data_off, obj->data.p, obj->data.len);

  /* relocations: executables resolve internal targets in place and send
     imports to .rela.dyn; shared objects send everything to .rela.dyn --
     internal targets as R_X86_64_RELATIVE (loader writes bias + addend) */
  {
    Elf64_Rela *er = (Elf64_Rela *) (p + rela_off);
    if (!shared_p) {
      er->r_offset = text_vaddr + OBJX_LSM_SLOT;
      er->r_info = ELF64_R_INFO ((uint64_t) lsm_idx, R_X86_64_64);
      er->r_addend = 0;
      er++;
    }
    for (size_t i = 0; i < obj->n_rels; i++) {
      objreloc_t *r = &obj->rels[i];
      uint64_t slot_off = (r->sec == MIR_OBJ_SEC_TEXT ? text_off + text_bias : data_off)
                          + r->offset;
      objsym_t *s = &obj->syms[r->sym];
      if (s->defined_p && shared_p) {
        er->r_offset = base + slot_off;
        er->r_info = ELF64_R_INFO (0, R_X86_64_RELATIVE);
        er->r_addend = (int64_t) (OBJX_SEC_VADDR (s->sec) + s->value + (uint64_t) r->addend);
        er++;
      } else if (s->defined_p) {
        uint64_t v = OBJX_SEC_VADDR (s->sec) + s->value + (uint64_t) r->addend;
        memcpy (p + slot_off, &v, 8);
      } else {
        er->r_offset = base + slot_off;
        er->r_info = ELF64_R_INFO ((uint64_t) dyn_idx[r->sym], R_X86_64_64);
        er->r_addend = r->addend;
        er++;
      }
    }
  }

  { /* .dynamic */
    Elf64_Dyn *d = (Elf64_Dyn *) (p + dynamic_off);
    for (size_t i = 0; i < params->n_needed; i++) {
      d->d_tag = DT_NEEDED;
      d->d_un.d_val = lib_name_off[i];
      d++;
    }
#define OBJX_DYN(tag, val)  \
  do {                      \
    d->d_tag = (tag);       \
    d->d_un.d_val = (val);  \
    d++;                    \
  } while (0)
    if (params->runpath != NULL) OBJX_DYN (DT_RUNPATH, runpath_off);
    if (init_i < n) OBJX_DYN (DT_INIT, code_vaddr + obj->syms[init_i].value);
    OBJX_DYN (DT_STRTAB, base + dynstr_off);
    OBJX_DYN (DT_STRSZ, dynstr.len);
    OBJX_DYN (DT_SYMTAB, base + dynsym_off);
    OBJX_DYN (DT_SYMENT, sizeof (Elf64_Sym));
    OBJX_DYN (DT_HASH, base + hash_off);
    OBJX_DYN (DT_RELA, base + rela_off);
    OBJX_DYN (DT_RELASZ, rela_size);
    OBJX_DYN (DT_RELAENT, sizeof (Elf64_Rela));
    OBJX_DYN (DT_TEXTREL, 0);
    OBJX_DYN (DT_NULL, 0);
#undef OBJX_DYN
  }

  memcpy (p + symtab_off, symtab.p, symtab.len);
  memcpy (p + strtab_off, strtab.p, strtab.len);
  memcpy (p + shstr_off, shstr.p, shstr.len);

  { /* ELF header */
    Elf64_Ehdr *eh = (Elf64_Ehdr *) p;
    eh->e_ident[EI_MAG0] = ELFMAG0;
    eh->e_ident[EI_MAG1] = ELFMAG1;
    eh->e_ident[EI_MAG2] = ELFMAG2;
    eh->e_ident[EI_MAG3] = ELFMAG3;
    eh->e_ident[EI_CLASS] = ELFCLASS64;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_ident[EI_OSABI] = ELFOSABI_SYSV;
    eh->e_type = shared_p ? ET_DYN : ET_EXEC;
    eh->e_machine = EM_X86_64;
    eh->e_version = EV_CURRENT;
    eh->e_entry = shared_p ? 0 : text_vaddr; /* the _start stub heads .text */
    eh->e_phoff = phdr_off;
    eh->e_shoff = shoff;
    eh->e_ehsize = sizeof (Elf64_Ehdr);
    eh->e_phentsize = sizeof (Elf64_Phdr);
    eh->e_phnum = (Elf64_Half) n_phdrs;
    eh->e_shentsize = sizeof (Elf64_Shdr);
    eh->e_shnum = (Elf64_Half) n_secs;
    eh->e_shstrndx = 12;
  }
  { /* program headers: LOAD R+X, LOAD R+W, [INTERP,] DYNAMIC */
    Elf64_Phdr *ph = (Elf64_Phdr *) (p + phdr_off);
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R | PF_X;
    ph[0].p_offset = 0;
    ph[0].p_vaddr = ph[0].p_paddr = base;
    ph[0].p_filesz = ph[0].p_memsz = rx_filesz;
    ph[0].p_align = OBJX_PAGE;
    ph[1].p_type = PT_LOAD;
    ph[1].p_flags = PF_R | PF_W;
    ph[1].p_offset = rw_off;
    ph[1].p_vaddr = ph[1].p_paddr = base + rw_off;
    ph[1].p_filesz = rw_filesz;
    ph[1].p_memsz = rw_memsz;
    ph[1].p_align = OBJX_PAGE;
    Elf64_Phdr *pd = &ph[2];
    if (!shared_p) {
      ph[2].p_type = PT_INTERP;
      ph[2].p_flags = PF_R;
      ph[2].p_offset = interp_off;
      ph[2].p_vaddr = ph[2].p_paddr = base + interp_off;
      ph[2].p_filesz = ph[2].p_memsz = interp_size;
      ph[2].p_align = 1;
      pd = &ph[3];
    }
    pd->p_type = PT_DYNAMIC;
    pd->p_flags = PF_R | PF_W;
    pd->p_offset = dynamic_off;
    pd->p_vaddr = pd->p_paddr = base + dynamic_off;
    pd->p_filesz = pd->p_memsz = dynamic_size;
    pd->p_align = 8;
  }
  { /* section headers (readelf/gdb view; the loader uses only phdrs) */
    Elf64_Shdr *sh = (Elf64_Shdr *) (p + shoff);
    struct {
      uint32_t type, link, info, align;
      uint64_t flags, off, vaddr_p, sz, entsize;
    } T[13] = {
      {0, 0, 0, 0, 0, 0, 0, 0, 0},
      {SHT_PROGBITS, 0, 0, 1, SHF_ALLOC, interp_off, 1, interp_size, 0},
      {SHT_HASH, 3, 0, 8, SHF_ALLOC, hash_off, 1, hash.len, 4},
      {SHT_DYNSYM, 4, 1, 8, SHF_ALLOC, dynsym_off, 1, dynsym.len, sizeof (Elf64_Sym)},
      {SHT_STRTAB, 0, 0, 1, SHF_ALLOC, dynstr_off, 1, dynstr.len, 0},
      {SHT_RELA, 3, 0, 8, SHF_ALLOC, rela_off, 1, rela_size, sizeof (Elf64_Rela)},
      {SHT_PROGBITS, 0, 0, 16, SHF_ALLOC | SHF_EXECINSTR, text_off, 1, text_size, 0},
      {SHT_PROGBITS, 0, 0, (uint32_t) data_align, SHF_ALLOC | SHF_WRITE, data_off, 1,
       obj->data.len, 0},
      {SHT_DYNAMIC, 4, 0, 8, SHF_ALLOC | SHF_WRITE, dynamic_off, 1, dynamic_size,
       sizeof (Elf64_Dyn)},
      {SHT_NOBITS, 0, 0, (uint32_t) bss_align, SHF_ALLOC | SHF_WRITE, off, 2, obj->bss_size, 0},
      {SHT_SYMTAB, 11, n_locals + 1, 8, 0, symtab_off, 0, symtab.len, sizeof (Elf64_Sym)},
      {SHT_STRTAB, 0, 0, 1, 0, strtab_off, 0, strtab.len, 0},
      {SHT_STRTAB, 0, 0, 1, 0, shstr_off, 0, shstr.len, 0},
    };
    for (int i = 1; i < n_secs; i++) {
      sh[i].sh_name = shname[i];
      sh[i].sh_type = T[i].type;
      sh[i].sh_flags = T[i].flags;
      sh[i].sh_addr = T[i].vaddr_p == 1 ? base + T[i].off
                      : T[i].vaddr_p == 2 ? bss_vaddr
                                          : 0;
      sh[i].sh_offset = T[i].off;
      sh[i].sh_size = T[i].sz;
      sh[i].sh_link = T[i].link;
      sh[i].sh_info = T[i].info;
      sh[i].sh_addralign = T[i].align;
      sh[i].sh_entsize = T[i].entsize;
    }
  }
  *buf = p;
  *size = (size_t) total;
  p = NULL;
  rc = 0;
#undef OBJX_SEC_VADDR
#undef OBJX_ALIGN

done:
  free (p);
  free (lib_name_off);
  free (dyn_idx);
  free (dynstr.p);
  free (dynsym.p);
  free (hash.p);
  free (symtab.p);
  free (strtab.p);
  free (shstr.p);
  return rc;
}
#else
int MIR_debug_emit (MIR_debug_t d MIR_UNUSED, void **buf, size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  return -1; /* no <elf.h> on this host */
}

MIR_object_t MIR_object_create (void) { return NULL; }
void MIR_object_destroy (MIR_object_t obj MIR_UNUSED) {}
size_t MIR_object_text_append (MIR_object_t obj MIR_UNUSED, const void *bytes MIR_UNUSED,
                               size_t len MIR_UNUSED) {
  return 0;
}
size_t MIR_object_data_append (MIR_object_t obj MIR_UNUSED, const void *bytes MIR_UNUSED,
                               size_t len MIR_UNUSED, size_t align MIR_UNUSED) {
  return 0;
}
size_t MIR_object_bss_reserve (MIR_object_t obj MIR_UNUSED, size_t len MIR_UNUSED,
                               size_t align MIR_UNUSED) {
  return 0;
}
int MIR_object_add_symbol (MIR_object_t obj MIR_UNUSED, const char *name MIR_UNUSED,
                           int sec MIR_UNUSED, uint64_t value MIR_UNUSED,
                           uint64_t size MIR_UNUSED, int func_p MIR_UNUSED,
                           int local_p MIR_UNUSED, int weak_p MIR_UNUSED) {
  return -1;
}
void MIR_object_symbol_define (MIR_object_t obj MIR_UNUSED, int sym_id MIR_UNUSED,
                               int sec MIR_UNUSED, uint64_t value MIR_UNUSED,
                               uint64_t size MIR_UNUSED, int func_p MIR_UNUSED,
                               int local_p MIR_UNUSED, int weak_p MIR_UNUSED) {}
int MIR_object_section_symbol (MIR_object_t obj MIR_UNUSED, int sec MIR_UNUSED) { return -1; }
int MIR_object_symbol_defined_p (MIR_object_t obj MIR_UNUSED, int sym_id MIR_UNUSED) { return 0; }
void MIR_object_add_reloc (MIR_object_t obj MIR_UNUSED, int sec MIR_UNUSED,
                           uint64_t offset MIR_UNUSED, int sym_id MIR_UNUSED,
                           int64_t addend MIR_UNUSED, int kind MIR_UNUSED) {}
int MIR_object_emit (MIR_object_t obj MIR_UNUSED, void **buf, size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  return -1;
}
int MIR_object_emit_executable (MIR_object_t obj MIR_UNUSED,
                                const MIR_object_exec_params *params MIR_UNUSED, void **buf,
                                size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  return -1;
}
#endif

/* The GDB JIT interface (the process-global __jit_debug_descriptor and
   MIR_debug_gdb_register/unregister) lives in mir-debug-gdb.c -- a separate
   translation unit so embedders that own their own descriptor can link this
   builder + MIR_debug_emit without a second one. */
