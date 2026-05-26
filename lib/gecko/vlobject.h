/* This file is a part of Gecko (GLR parser) project.
   Copyright (C) 2026 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef __VLO__
#define __VLO__

#include <stdlib.h>
#include <string.h>

#include "allocate.h"

#include <assert.h>

/* Default initial size of memory is allocated for VLO when the object
   is created (with zero initial size).  This macro can be redefined
   in C compiler command line or with the aid of directive `#undef'
   before any using the package macros. */

#ifndef VLO_DEFAULT_LENGTH
#define VLO_DEFAULT_LENGTH 512
#endif

/* This type describes a descriptor of variable length object.  All work with variable length object
   is executed by following macros through the descriptors.  Structure (implementation) of this type
   is not needed for using variable length object.  But it should remember that work with the object
   through several descriptors is not safe. */
typedef struct {
  char *vlo_start; /* pointer to memory currently used for storing the VLO */
  char *vlo_free;  /* pointer to first byte after the last VLO byte */
  /* pointer to first byte after the memory currently allocated for storing the VLO: */
  char *vlo_boundary;
  gp_allocator_t *vlo_alloc; /* pointer to allocator */
} vlo_t;

/* This macro is used for creation of VLO with initial zero length. If initial length of memory
   needed for the VLO is equal to 0 the initial allocated memory length is equal to
   VLO_DEFAULT_LENGTH. VLO must be created before any using other macros of the package for work
   with given VLO.  The macro has not side effects. */
#define VLO_CREATE(vlo, allocator, initial_length)                                               \
  do {                                                                                           \
    vlo_t *_temp_vlo = &(vlo);                                                                   \
    size_t temp_initial_length = (initial_length);                                               \
    gp_allocator_t *_temp_alloc = (allocator);                                                   \
    temp_initial_length = (temp_initial_length != 0 ? temp_initial_length : VLO_DEFAULT_LENGTH); \
    _temp_vlo->vlo_start = (char *) gp_malloc (_temp_alloc, temp_initial_length);                \
    _temp_vlo->vlo_boundary = _temp_vlo->vlo_start + temp_initial_length;                        \
    _temp_vlo->vlo_free = _temp_vlo->vlo_start;                                                  \
    _temp_vlo->vlo_alloc = _temp_alloc;                                                          \
  } while (0)

/* This macro is used for freeing memory allocated for VLO.  Any work (except for creation) with
   given VLO is not possible after evaluation of this macro.  The macro has not side effects. */
#ifndef NDEBUG
#define VLO_DELETE(vlo)                                   \
  do {                                                    \
    vlo_t *_temp_vlo = &(vlo);                            \
    assert (_temp_vlo->vlo_start != NULL);                \
    gp_free (_temp_vlo->vlo_alloc, _temp_vlo->vlo_start); \
    _temp_vlo->vlo_start = NULL;                          \
  } while (0)
#else
#define VLO_DELETE(vlo)                                   \
  do {                                                    \
    vlo_t *_temp_vlo = &(vlo);                            \
    gp_free (_temp_vlo->vlo_alloc, _temp_vlo->vlo_start); \
  } while (0)
#endif /* #ifndef NDEBUG */

/* This macro makes that length of VLO will be equal to zero (but memory for VLO is not freed and
   not reallocated).  The macro has not side effects. */
#define VLO_NULLIFY(vlo)                        \
  do {                                          \
    vlo_t *_temp_vlo = &(vlo);                  \
    assert (_temp_vlo->vlo_start != NULL);      \
    _temp_vlo->vlo_free = _temp_vlo->vlo_start; \
  } while (0)

/* The following macro makes that length of memory allocated for VLO becames equal to VLO length.
   The macro has not side effects. */
#define VLO_TAILOR(vlo) _VLO_tailor_function (&(vlo))

/* This macro returns current length of VLO.  The macro has side effects! */
#ifndef NDEBUG
#define VLO_LENGTH(vlo) \
  ((vlo).vlo_start != NULL ? (size_t) ((vlo).vlo_free - (vlo).vlo_start) : (abort (), (size_t) 0))
#else
#define VLO_LENGTH(vlo) ((size_t) ((vlo).vlo_free - (vlo).vlo_start))
#endif /* #ifndef NDEBUG */

/* This macro returns pointer (of type `void *') to the first byte of the VLO.  The macro has side
   effects!  Remember also that the VLO may change own place after any addition. */
#ifndef NDEBUG
#define VLO_BEGIN(vlo) ((vlo).vlo_start != NULL ? (void *) (vlo).vlo_start : (abort (), (void *) 0))
#else
#define VLO_BEGIN(vlo) ((void *) (vlo).vlo_start)
#endif /* #ifndef NDEBUG */

/* This macro returns pointer (of type `void *') to the last byte of VLO.  The macro has side
   effects!  Remember also that the VLO may change own place after any addition. */
#ifndef NDEBUG
#define VLO_END(vlo) ((vlo).vlo_start != NULL ? (void *) ((vlo).vlo_free - 1) : (abort (), (void *) 0))
#else
#define VLO_END(vlo) ((void *) ((vlo).vlo_free - 1))
#endif /* #ifndef NDEBUG */

/* This macro returns pointer (of type `void *') to the next byte of the last byte of VLO.  The
   macro has side effects!  Remember also that the VLO may change own place after any addition. */
#ifndef NDEBUG
#define VLO_BOUND(vlo) ((vlo).vlo_start != NULL ? (void *) (vlo).vlo_free : (abort (), (void *) 0))
#else
#define VLO_BOUND(vlo) ((void *) (vlo).vlo_free)
#endif /* #ifndef NDEBUG */

/* This macro removes N bytes from the end of VLO.  VLO is nullified if its length is less than N.
   The macro has not side effects. */
#define VLO_SHORTEN(vlo, n)                         \
  do {                                              \
    vlo_t *_temp_vlo = &(vlo);                      \
    size_t _temp_n = (n);                           \
    assert (_temp_vlo->vlo_start != NULL);          \
    if ((size_t) VLO_LENGTH (*_temp_vlo) < _temp_n) \
      _temp_vlo->vlo_free = _temp_vlo->vlo_start;   \
    else                                            \
      _temp_vlo->vlo_free -= _temp_n;               \
  } while (0)

/* This macro increases length of VLO.  The values of bytes added to the end of VLO will be not
   defined.  The macro has not side effects. */
#define VLO_EXPAND(vlo, length)                                       \
  do {                                                                \
    vlo_t *_temp_vlo = &(vlo);                                        \
    size_t _temp_length = (length);                                   \
    assert (_temp_vlo->vlo_start != NULL);                            \
    if (_temp_vlo->vlo_free + _temp_length > _temp_vlo->vlo_boundary) \
      _VLO_expand_memory (_temp_vlo, _temp_length);                   \
    _temp_vlo->vlo_free += _temp_length;                              \
  } while (0)

/* This macro adds a byte to the end of VLO.  The macro has not side effects. */
#define VLO_ADD_BYTE(vlo, b)                                                               \
  do {                                                                                     \
    vlo_t *_temp_vlo = &(vlo);                                                             \
    assert (_temp_vlo->vlo_start != NULL);                                                 \
    if (_temp_vlo->vlo_free >= _temp_vlo->vlo_boundary) _VLO_expand_memory (_temp_vlo, 1); \
    *_temp_vlo->vlo_free++ = (b);                                                          \
  } while (0)

/* This macro adds memory bytes to the end of VLO.  The macro has not side effects. */
#define VLO_ADD_MEMORY(vlo, str, length)                              \
  do {                                                                \
    vlo_t *_temp_vlo = &(vlo);                                        \
    size_t _temp_length = (length);                                   \
    assert (_temp_vlo->vlo_start != NULL);                            \
    if (_temp_vlo->vlo_free + _temp_length > _temp_vlo->vlo_boundary) \
      _VLO_expand_memory (_temp_vlo, _temp_length);                   \
    memcpy (_temp_vlo->vlo_free, (str), _temp_length);                \
    _temp_vlo->vlo_free += _temp_length;                              \
  } while (0)

/* This macro adds C string (with end marker '\0') to the end of VLO. Before the addition the macro
   delete last character of the VLO. The last character is suggested to be C string end marker '\0'.
   The macro has not side effects. */
#define VLO_ADD_STRING(vlo, str) _VLO_add_string_function (&(vlo), (str))

/* The following functions are to be used only by the package macros. Remember that they are
   internal functions - all work with VLO is executed through the macros. */

/* The function implements macro `VLO_TAILOR'.  Length of memory allocated for VLO becames equal to
   VLO length (but memory for zero length object will contain one byte).  Remember that the VLO
   place may be changed after the call. */
static inline void _VLO_tailor_function (vlo_t *vlo) {
  size_t vlo_length;
  char *new_vlo_start;

  assert (vlo->vlo_start != NULL);
  vlo_length = VLO_LENGTH (*vlo);
  if (vlo_length == 0) vlo_length = 1;
  new_vlo_start = (char *) gp_realloc (vlo->vlo_alloc, vlo->vlo_start, vlo_length);
  if (new_vlo_start != vlo->vlo_start) {
    vlo->vlo_free += new_vlo_start - vlo->vlo_start;
    vlo->vlo_start = new_vlo_start;
  }
  vlo->vlo_boundary = vlo->vlo_start + vlo_length;
}

/* The following function changes size of memory allocated for VLO. The size becames equal to about
   one and a half of VLO length accounting for length of memory which will be added after the call.
   Remember that the VLO place may be changed after the call. */
static inline void _VLO_expand_memory (vlo_t *vlo, size_t additional_length) {
  size_t vlo_length;
  char *new_vlo_start;

  assert (vlo->vlo_start != NULL);
  vlo_length = VLO_LENGTH (*vlo) + additional_length;
  vlo_length += vlo_length / 2 + 1;
  new_vlo_start = (char *) gp_realloc (vlo->vlo_alloc, vlo->vlo_start, vlo_length);
  if (new_vlo_start != vlo->vlo_start) {
    vlo->vlo_free += new_vlo_start - vlo->vlo_start;
    vlo->vlo_start = new_vlo_start;
  }
  vlo->vlo_boundary = vlo->vlo_start + vlo_length;
}

/* The following function implements macro `VLO_ADD_STRING' (addition of string STR (with end marker
   is '\0') to the end of VLO). Remember that the VLO place may be changed after the call. */
static inline void _VLO_add_string_function (vlo_t *vlo, const char *str) {
  size_t length;

  assert (vlo->vlo_start != NULL);
  if (str == NULL) return;
  if (vlo->vlo_free != vlo->vlo_start) VLO_SHORTEN (*vlo, 1);
  length = strlen (str) + 1;
  if (vlo->vlo_free + length > vlo->vlo_boundary) _VLO_expand_memory (vlo, length);
  memcpy (vlo->vlo_free, str, length);
  vlo->vlo_free = vlo->vlo_free + length;
}

#endif /* #ifndef __VLO__ */
