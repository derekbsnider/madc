/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include "../mirc.h"
#include "mirc_aarch64_linux.h"

#include "mirc_aarch64_float.h"
#include "mirc_aarch64_limits.h"
#include "mirc_aarch64_stdarg.h"
#include "mirc_aarch64_stdint.h"
#include "mirc_aarch64_stddef.h"

static string_include_t standard_includes[]
  = {{NULL, mirc}, {NULL, aarch64_mirc}, TARGET_STD_INCLUDES};

#define MAX_ALIGNMENT 16

#define ADJUST_VAR_ALIGNMENT(c2m_ctx, align, type) \
  aarch64_adjust_var_alignment (c2m_ctx, align, type)

static int aarch64_adjust_var_alignment (c2m_ctx_t c2m_ctx MIR_UNUSED, int align,
                                         struct type *type MIR_UNUSED) {
  return align;
}

/* madc fork: an object's alignment may exceed MAX_ALIGNMENT up to a page
   (gcc's aligned(N), C11 _Alignas(N), a wide vector type): static storage
   carries it on its MIR item (MIR_item_set_align); a frame holding such an
   object is realigned at function entry (node_scope frame_align). */
#define MAX_STATIC_ALIGNMENT 4096

static int invalid_alignment (mir_llong align) {
  return align < 0 || align > MAX_STATIC_ALIGNMENT || (align & (align - 1)) != 0;
}
