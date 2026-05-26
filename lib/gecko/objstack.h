/* This file is a part of Gecko (GLR parser) project.
   Copyright (C) 2026 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef __OS__
#define __OS__

#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "allocate.h"

#include <assert.h>

/* This auxiliary structure is used to evaluation of maximum alignment for objects: */
struct _os_auxiliary_struct {
  char _os_character;
  double _os_double;
};

/* This macro is auxiliary.  Its value is maximum alignment for objects. */
#ifdef VMS
/* It is necessarry for VMS C compiler. */
#define _OS_ALIGNMENT 4
#else
#define _OS_ALIGNMENT offsetof (struct _os_auxiliary_struct, _os_double)
#if 0
#define _OS_ALIGNMENT ((char *) &((struct _os_auxiliary_struct *) 64)->_os_double - (char *) 64)
#else
#endif
#endif

/* This macro is auxiliary.  Its value is aligned address nearest to `a'. */
#define _OS_ALIGNED_ADDRESS(a) \
  ((char *) ((size_t) ((char *) (a) + (_OS_ALIGNMENT - 1)) & (~(size_t) (_OS_ALIGNMENT - 1))))

/* This macro value is default size of memory segments which will be allocated for OS when the stack
   is created (with zero initial segment size).  This is also minimal size of all segments. Original
   value of the macros is equal to 512.  This macro can be redefined in C compiler command line or
   with the aid of directive `#undef' before any using the package macros.  */
#ifndef OS_DEFAULT_SEGMENT_LENGTH
#define OS_DEFAULT_SEGMENT_LENGTH 512
#endif

/* This internal structure describes segment of an object stack. */
struct _os_segment {
  struct _os_segment *os_previous_segment;
  char os_segment_contest[_OS_ALIGNMENT];
};

/* This type describes a descriptor of stack of objects.  All work with stack of objects is executed
   by following macros through the descriptors.  Structure (implementation) of this type is not
   needed for using stack of objects.  But it should remember that work with the stack through
   several descriptors is not safe. */
typedef struct {
  size_t initial_segment_length; /* the real length of the first memory segment */
  struct _os_segment *os_current_segment;
  char *os_top_object_start; /* pointer to memory currently used for storing the top object */
  char *os_top_object_free;  /* pointer to first byte after the last byte of the top object */
  /* pointer to 1st byte after the memory allocated for storing OS segment and the top object: */
  char *os_boundary;
  gp_allocator_t *os_alloc; /* pointer to allocator */
} os_t;

/* This macro creates OS which contains the single zero length object. If initial length of OS
   segment is equal to zero the allocated memory segments length is equal to
   `OS_DEFAULT_SEGMENT_LENGTH'. But in any case the segment length is always not less than maximum
   alignment.  OS must be created before any using other macros of the package for work with given
   OS.  The macro has not side effects. */
#define OS_CREATE(os, allocator, initial_segment_length)    \
  do {                                                      \
    os_t *_temp_os = &(os);                                 \
    _temp_os->os_alloc = allocator;                         \
    _OS_create_function (_temp_os, initial_segment_length); \
  } while (0)

/* This macro is used for freeing memory allocated for OS.  Any work (except for creation) with
   given OS is not possible after evaluation of this macros.  The macro has not side effects. */
#define OS_DELETE(os) _OS_delete_function (&(os))

/* This macro is used for freeing memory allocated for OS except for the first segment.  The macro
   has not side effects. */
#define OS_EMPTY(os) _OS_empty_function (&(os))

/* This macro makes that length of variable length object on the top of OS will be equal to zero.
   The macro has not side effects. */
#define OS_TOP_NULLIFY(os)                                        \
  do {                                                            \
    os_t *_temp_os = &(os);                                       \
    assert (_temp_os->os_top_object_start != NULL);               \
    _temp_os->os_top_object_free = _temp_os->os_top_object_start; \
  } while (0)

/* The macro creates new variable length object with initial zero length on the top of OS .  The
   work (analogous to one with variable length object) with object which was on the top of OS are
   finished, i.e. the object will never more change address.  The macro has not side effects. */
#define OS_TOP_FINISH(os)                                                               \
  do {                                                                                  \
    os_t *_temp_os = &(os);                                                             \
    assert (_temp_os->os_top_object_start != NULL);                                     \
    _temp_os->os_top_object_start = _OS_ALIGNED_ADDRESS (_temp_os->os_top_object_free); \
    _temp_os->os_top_object_free = _temp_os->os_top_object_start;                       \
  } while (0)

/* This macro returns current length of variable length object on the top of OS.  The macro has side
   effects! */
#ifndef NDEBUG
#define OS_TOP_LENGTH(os)                                                                           \
  ((os).os_top_object_start != NULL ? (size_t) ((os).os_top_object_free - (os).os_top_object_start) \
                                    : (abort (), (size_t) 0))
#else
#define OS_TOP_LENGTH(os) ((os).os_top_object_free - (os).os_top_object_start)
#endif

/* This macro returns pointer to the first byte of variable length object on the top of OS.  The
   macro has side effects!  Remember also that the top object may change own place after any
   addition. */
#ifndef NDEBUG
#define OS_TOP_BEGIN(os) \
  ((os).os_top_object_start != NULL ? (void *) (os).os_top_object_start : (abort (), (void *) 0))
#else
#define OS_TOP_BEGIN(os) ((void *) (os).os_top_object_start)
#endif

/* This macro returns pointer (of type `void *') to the last byte of variable length object on the
   top OS.  The macro has side effects! Remember also that the top object may change own place after
   any addition. */
#ifndef NDEBUG
#define OS_TOP_END(os) \
  ((os).os_top_object_start != NULL ? (void *) ((os).os_top_object_free - 1) : (abort (), (void *) 0))
#else
#define OS_TOP_END(os) ((void *) ((os).os_top_object_free - 1))
#endif

/* This macro returns pointer (of type `void *') to the next byte of the last byte of variable
   length object on the top OS.  The macro has side effects!  Remember also that the top object may
   change own place after any addition. */
#ifndef NDEBUG
#define OS_TOP_BOUND(os) \
  ((os).os_top_object_start != NULL ? (void *) (os).os_top_object_free : (abort (), (void *) 0))
#else
#define OS_TOP_BOUND(os) ((void *) (os).os_top_object_free)
#endif

/* This macro removes N bytes from the end of variable length object on the top of OS.  The top
   variable length object is nullified if its length is less than N.  The macro has not side
   effects. */
#define OS_TOP_SHORTEN(os, n)                                       \
  do {                                                              \
    os_t *_temp_os = &(os);                                         \
    size_t _temp_n = (n);                                           \
    assert (_temp_os->os_top_object_start != NULL);                 \
    if ((size_t) OS_TOP_LENGTH (*_temp_os) < _temp_n)               \
      _temp_os->os_top_object_free = _temp_os->os_top_object_start; \
    else                                                            \
      _temp_os->os_top_object_free -= _temp_n;                      \
  } while (0)

/* This macro increases length of variable length object on the top of OS on given number of bytes.
   The values of bytes added to the end of variable length object on the top of OS will be not
   defined. The macro has not side effects. */
#define OS_TOP_EXPAND(os, length)                                            \
  do {                                                                       \
    os_t *_temp_os = &(os);                                                  \
    size_t _temp_length = (length);                                          \
    assert (_temp_os->os_top_object_start != NULL);                          \
    if (_temp_os->os_top_object_free + _temp_length > _temp_os->os_boundary) \
      _OS_expand_memory (_temp_os, _temp_length);                            \
    _temp_os->os_top_object_free += _temp_length;                            \
  } while (0)

/* This macro adds byte to the end of variable length object on the top of OS.  The macro has not
   side effects. */
#define OS_TOP_ADD_BYTE(os, b)                                                                  \
  do {                                                                                          \
    os_t *_temp_os = &(os);                                                                     \
    assert (_temp_os->os_top_object_start != NULL);                                             \
    if (_temp_os->os_top_object_free >= _temp_os->os_boundary) _OS_expand_memory (_temp_os, 1); \
    *_temp_os->os_top_object_free++ = (b);                                                      \
  } while (0)

/* This macro adds memory bytes to the end of variable length object on the top of OS.  The macro
   has not side effects. */
#define OS_TOP_ADD_MEMORY(os, str, length)                                   \
  do {                                                                       \
    os_t *_temp_os = &(os);                                                  \
    size_t _temp_length = (length);                                          \
    assert (_temp_os->os_top_object_start != NULL);                          \
    if (_temp_os->os_top_object_free + _temp_length > _temp_os->os_boundary) \
      _OS_expand_memory (_temp_os, _temp_length);                            \
    memcpy (_temp_os->os_top_object_free, (str), _temp_length);              \
    _temp_os->os_top_object_free += _temp_length;                            \
  } while (0)

/* This macro adds C string (with end marker '\0') to the end of variable length object on the top
   of OS.  Before the addition the macro delete last character of the object.  The last character is
   suggested to be C string end marker '\0'.  The macro has not side effects. */
#define OS_TOP_ADD_STRING(os, str) _OS_add_string_function (&(os), (str))

/* The following functions are to be used only by the package macros. Remember that they are
   internal functions - all work with OS is executed through the macros. */

/* The function implements macro `OS_CREATE' (creation of stack of object).  OS must be created
   before any using other macros of the package for work with given OS. */
static inline void _OS_create_function (os_t *os, size_t initial_segment_length) {
  if (initial_segment_length == 0) initial_segment_length = OS_DEFAULT_SEGMENT_LENGTH;
  os->os_current_segment
    = (struct _os_segment *) gp_malloc (os->os_alloc, initial_segment_length + sizeof (struct _os_segment));
  os->os_current_segment->os_previous_segment = NULL;
  os->os_top_object_start = _OS_ALIGNED_ADDRESS (os->os_current_segment->os_segment_contest);
  os->os_top_object_free = os->os_top_object_start;
  os->os_boundary = os->os_top_object_start + initial_segment_length;
  os->initial_segment_length = initial_segment_length;
}

/* The function implements macro `OS_DELETE' (freeing memory allocated for OS).  Any work (except
   for creation) with given OS is not possible after evaluation of this macros.  The macro has not
   side effects. */
static inline void _OS_delete_function (os_t *os) {
  struct _os_segment *current_segment, *previous_segment;

  assert (os->os_top_object_start != NULL);
  os->os_top_object_start = NULL;
  for (current_segment = os->os_current_segment; current_segment != NULL;
       current_segment = previous_segment) {
    previous_segment = current_segment->os_previous_segment;
    gp_free (os->os_alloc, current_segment);
  }
}

/* The function implements macro `OS_EMPTY' (freeing memory allocated for OS except for the first
   segment). */
static inline void _OS_empty_function (os_t *os) {
  struct _os_segment *current_segment, *previous_segment;

  assert (os->os_top_object_start != NULL && os->os_current_segment != NULL);
  current_segment = os->os_current_segment;
  for (;;) {
    previous_segment = current_segment->os_previous_segment;
    if (previous_segment == NULL) break;
    gp_free (os->os_alloc, current_segment);
    current_segment = previous_segment;
  }
  os->os_current_segment = current_segment;
  os->os_top_object_start = _OS_ALIGNED_ADDRESS (current_segment->os_segment_contest);
  os->os_top_object_free = os->os_top_object_start;
  os->os_boundary = os->os_top_object_start + os->initial_segment_length;
}

/* The function creates new segment for OS.  The segment becames current and its size becames equal
   to about one and a half of the top object length accounting for length of memory which will be
   added after the call (but not less than the default segment length).  The function deletes the
   segment which was current if the segment contained only the top object.  Remember that the top
   object place may be changed after the call. */
static inline void _OS_expand_memory (os_t *os, size_t additional_length) {
  size_t os_top_object_length, segment_length;
  struct _os_segment *new_segment, *previous_segment;
  char *new_os_top_object_start;

  assert (os->os_top_object_start != NULL);
  os_top_object_length = OS_TOP_LENGTH (*os);
  segment_length = os_top_object_length + additional_length;
  segment_length += segment_length / 2 + 1;
  if (segment_length < OS_DEFAULT_SEGMENT_LENGTH) segment_length = OS_DEFAULT_SEGMENT_LENGTH;
  new_segment = (struct _os_segment *) gp_malloc (os->os_alloc, segment_length + sizeof (struct _os_segment));
  new_os_top_object_start = _OS_ALIGNED_ADDRESS (new_segment->os_segment_contest);
  memcpy (new_os_top_object_start, os->os_top_object_start, os_top_object_length);
  if (os->os_top_object_start == _OS_ALIGNED_ADDRESS (os->os_current_segment->os_segment_contest)) {
    previous_segment = os->os_current_segment->os_previous_segment;
    gp_free (os->os_alloc, os->os_current_segment);
  } else
    previous_segment = os->os_current_segment;
  os->os_current_segment = new_segment;
  new_segment->os_previous_segment = previous_segment;
  os->os_top_object_start = new_os_top_object_start;
  os->os_top_object_free = os->os_top_object_start + os_top_object_length;
  os->os_boundary = os->os_top_object_start + segment_length;
}

/* The function implements macro `OS_ADD_STRING' (addition of string STR (with end marker is '\0')
   to the end of OS).  Remember that the OS place may be changed after the call. */
static inline void _OS_add_string_function (os_t *os, const char *str) {
  size_t string_length;

  assert (os->os_top_object_start != NULL);
  if (str == NULL) return;
  if (os->os_top_object_free != os->os_top_object_start) OS_TOP_SHORTEN (*os, 1);
  string_length = strlen (str) + 1;
  if (os->os_top_object_free + string_length > os->os_boundary) _OS_expand_memory (os, string_length);
  memcpy (os->os_top_object_free, str, string_length);
  os->os_top_object_free = os->os_top_object_free + string_length;
}

#endif /* #ifndef __OS__ */
