/* This file is a part of Gecko (GLR parser) project.
   Copyright (C) 2026 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef GP_ALLOCATE_H_
#define GP_ALLOCATE_H_

#include <stddef.h>
#include <stdlib.h>

#ifdef __GNUC__
#define ALLOC_UNUSED __attribute__ ((unused))
#else
#define ALLOC_UNUSED
#endif

typedef void *(*gp_malloc_t) (size_t);
typedef void *(*gp_calloc_t) (size_t, size_t);
typedef void *(*gp_realloc_t) (void *, size_t);
typedef void (*gp_free_t) (void *ptr);
typedef void (*gp_alloc_error_t) (void *userptr);

/* GP allocator type: */
struct gp_allocator {
  gp_malloc_t malloc;
  gp_calloc_t calloc;
  gp_realloc_t realloc;
  gp_free_t free;
  gp_alloc_error_t alloc_error;
  void *userptr;
};
typedef struct gp_allocator gp_allocator_t;

/* Default error handling function. */
static inline void gp_alloc_defaulterrfunc (void *ignored) {
  (void) ignored;
  fputs ("*** out of memory ***\n", stderr);
  exit (1);
}

/* Creates a new allocator: null arg means using default implementation. On error, a null pointer is
 * returned. */
static inline struct gp_allocator *gp_alloc_new (gp_malloc_t mallocf, gp_calloc_t callocf,
                                                 gp_realloc_t reallocf, gp_free_t freef) {
  struct gp_allocator *result;

  /* Sanity checks */
  if (mallocf == NULL) mallocf = malloc;
  if ((callocf == NULL) && (mallocf == malloc)) callocf = calloc;
  if (reallocf == NULL) {
    if ((mallocf == malloc) && (callocf == calloc))
      reallocf = realloc;
    else
      return NULL;
  }
  if (freef == NULL) {
    if ((mallocf == malloc) && (callocf == calloc) && (reallocf == realloc))
      freef = free;
    else
      return NULL;
  }

  /* Allocate allocator */
  result = (struct gp_allocator *) mallocf (sizeof (*result));
  if (result == NULL) return NULL;
  result->malloc = mallocf;
  result->calloc = callocf;
  result->realloc = reallocf;
  result->free = freef;
  result->alloc_error = gp_alloc_defaulterrfunc;
  result->userptr = result;

  return result;
}

/* Destroys an allocator: */
static inline void gp_alloc_del (struct gp_allocator *allocator) {
  if (allocator == NULL) return;
  gp_free_t freef = allocator->free;
  freef (allocator);
}

/* Allocates memory: */
static inline void *gp_malloc (struct gp_allocator *allocator, size_t size) {
  void *result;

  if (allocator == NULL) return NULL;
  result = allocator->malloc (size);
  if ((result == NULL) && (size != 0)) allocator->alloc_error (allocator->userptr);
  return result;
}

/* Allocates zero-initialised memory: */
static inline void *gp_calloc (struct gp_allocator *allocator, size_t nmemb, size_t size) {
  void *result;

  if (allocator == NULL) return NULL;
  if (allocator->calloc != NULL) {
    result = allocator->calloc (nmemb, size);
  } else if ((nmemb == 0) || (size == 0)) {
    result = NULL;
  } else {
    size_t total = nmemb * size;
    if (total / nmemb != size)
      result = NULL;
    else {
      result = allocator->malloc (total);
      if (result != NULL) memset (result, '\0', total);
    }
  }
  if ((result == NULL) && (nmemb != 0) && (size != 0)) allocator->alloc_error (allocator->userptr);
  return result;
}

/* Resizes memory: */
static inline void *gp_realloc (struct gp_allocator *allocator, void *ptr, size_t size) {
  void *result;

  if (allocator == NULL) return NULL;
  result = allocator->realloc (ptr, size);
  if ((result == NULL) && (size != 0)) allocator->alloc_error (allocator->userptr);
  return result;
}

/* Frees previously allocated memory: */
static inline void gp_free (struct gp_allocator *allocator, void *ptr) {
  if (allocator != NULL) allocator->free (ptr);
}

/* Obtains the current error function of an allocator: */
static gp_alloc_error_t ALLOC_UNUSED gp_alloc_geterrfunc (gp_allocator_t *allocator) {
  return allocator != NULL ? allocator->alloc_error : NULL;
}

/* Obtains the current user-provided pointer: */
static inline void *gp_alloc_getuserptr (gp_allocator_t *allocator) {
  return allocator != NULL ? allocator->userptr : NULL;
}

/* Sets the error function and user-provided pointer of an allocator. The error function is called
 * by the allocator with the user-provided pointer as argument whenever an allocation error occurs.
 */
static inline void gp_alloc_seterr (gp_allocator_t *allocator, gp_alloc_error_t errfunc,
                                    void *userptr) {
  if (allocator == NULL) return;
  if (errfunc != NULL)
    allocator->alloc_error = errfunc;
  else
    allocator->alloc_error = gp_alloc_defaulterrfunc;
  allocator->userptr = userptr;
}

#endif
