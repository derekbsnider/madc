#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mir.h"
#include "c2mir/c2mir.h"

/* Return a max-aligned address after the libc allocation.  Any c2mir path
   that bypasses MIR_free and calls raw free on it will abort this test.  */
union alloc_header {
  max_align_t alignment;
  size_t size;
};

struct alloc_state {
  size_t alloc_calls, free_calls;
};

static void *tracked_malloc (size_t size, void *data) {
  struct alloc_state *state = data;
  union alloc_header *header = malloc (sizeof (*header) + size);
  if (header == NULL) return NULL;
  header->size = size;
  state->alloc_calls++;
  return header + 1;
}

static void *tracked_calloc (size_t count, size_t size, void *data) {
  void *ptr;
  if (size != 0 && count > (size_t) -1 / size) return NULL;
  ptr = tracked_malloc (count * size, data);
  if (ptr != NULL) memset (ptr, 0, count * size);
  return ptr;
}

static void tracked_free (void *ptr, void *data) {
  struct alloc_state *state = data;
  if (ptr != NULL) {
    free ((union alloc_header *) ptr - 1);
    state->free_calls++;
  }
}

static void *tracked_realloc (void *ptr, size_t old_size, size_t new_size, void *data) {
  void *result;
  if (ptr == NULL) return tracked_malloc (new_size, data);
  if (new_size == 0) {
    tracked_free (ptr, data);
    return NULL;
  }
  result = tracked_malloc (new_size, data);
  if (result == NULL) return NULL;
  memcpy (result, ptr, old_size < new_size ? old_size : new_size);
  tracked_free (ptr, data);
  return result;
}

struct input {
  const char *cursor;
};

static int string_getc (void *data) {
  struct input *input = data;
  unsigned char c = (unsigned char) *input->cursor;
  if (c == 0) return EOF;
  input->cursor++;
  return c;
}

int main (void) {
  struct alloc_state state = {0};
  struct MIR_alloc alloc = {
    tracked_malloc, tracked_calloc, tracked_realloc, tracked_free, &state,
  };
  struct c2mir_options options;
  struct input input = {"int answer(void) { return 42; }\n"};
  MIR_context_t ctx;
  int ok;

  memset (&options, 0, sizeof (options));
  options.message_file = stderr;
  ctx = MIR_init2 (&alloc, NULL);
  c2mir_init (ctx);
  ok = c2mir_compile (ctx, &options, string_getc, &input, "custom-alloc.c", NULL);
  c2mir_finish (ctx);
  MIR_finish (ctx);
  if (!ok || state.free_calls == 0 || state.alloc_calls != state.free_calls) {
    fprintf (stderr, "ok=%d alloc=%zu free=%zu\n", ok, state.alloc_calls, state.free_calls);
    return 1;
  }
  return 0;
}
