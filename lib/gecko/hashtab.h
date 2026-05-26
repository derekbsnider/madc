/* This file is a part of Gecko (GLR parser) project.
   Copyright (C) 2026 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef __HASH_TABLE__
#define __HASH_TABLE__

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

/* The hash table element is represented by the following type. */
typedef const void *hash_table_entry_t;

/* Hash tables are of the following type.  The structure (implementation) of this type is not needed
   for using the hash tables.  All work with hash table should be executed only through functions
   mentioned below. */
typedef struct hash_table {
  size_t size;                       /* current size (in entries) of the hash table */
  size_t number_of_elements;         /* current number of elements including also deleted elements */
  size_t number_of_deleted_elements; /* current number of deleted elements in the table */
  /* The member is used for debugging. Its value is number of all calls of `find_hash_table_entry'
     for the hash table. */
  int searches;
  /* The member is used for debugging.  Its value is number of collisions fixed for time of work
     with the hash table. */
  int collisions;
  /* Pointer to function for evaluation of hash value (any unsigned value).
     This function has one parameter of type hash_table_entry_t. */
  uint64_t (*hash_function) (hash_table_entry_t el_ptr);
  /* Pointer to function for test on equality of hash table elements (two
     parameter of type hash_table_entry_t. */
  bool (*eq_function) (hash_table_entry_t el1_ptr, hash_table_entry_t el2_ptr);
  hash_table_entry_t *entries; /* table itself */
  gp_allocator_t *alloc;       /* allocator */
} *hash_table_t;

/* The following function returns number of searches during all work with given hash table. */
static inline int get_searches (hash_table_t htab) { return htab->searches; }

/* The following function returns number of occurred collisions during all work with given hash
   table. */
static inline int get_collisions (hash_table_t htab) { return htab->collisions; }

/* This macro defines reserved value for empty table entry. */
#define EMPTY_ENTRY NULL

/* This macro defines reserved value for table entry which contained a deleted element. */
#define DELETED_ENTRY ((void *) 1)

/* Create table with at least min_size length.  Created hash table is
   initiated as empty (all the hash table entries are EMPTY_ENTRY).  The function returns the
   created hash table. */
static inline hash_table_t create_hash_table (gp_allocator_t *allocator, size_t min_size,
                                              uint64_t (*hash_function) (hash_table_entry_t el_ptr),
                                              bool (*eq_function) (hash_table_entry_t el1_ptr,
                                                                   hash_table_entry_t el2_ptr)) {
  hash_table_t result;
  hash_table_entry_t *entry_ptr;
  size_t size;

  for (size = 2; min_size > size; size *= 2);
  result = (hash_table_t) gp_malloc (allocator, sizeof (*result));
  result->entries = (hash_table_entry_t *) gp_malloc (allocator, size * sizeof (hash_table_entry_t));
  result->size = size;
  result->hash_function = hash_function;
  result->eq_function = eq_function;
  result->number_of_elements = 0;
  result->number_of_deleted_elements = 0;
  result->searches = 0;
  result->collisions = 0;
  result->alloc = allocator;
  for (entry_ptr = result->entries; entry_ptr < result->entries + size; entry_ptr++) *entry_ptr = EMPTY_ENTRY;
  return result;
}

/* Make the table empty.  Naturally the hash table must already exist. */
static inline void empty_hash_table (hash_table_t htab) {
  hash_table_entry_t *entry_ptr;

  assert (htab != NULL);
  htab->number_of_elements = 0;
  htab->number_of_deleted_elements = 0;
  for (entry_ptr = htab->entries; entry_ptr < htab->entries + htab->size; entry_ptr++)
    *entry_ptr = EMPTY_ENTRY;
}

/* Free all memory allocated for given hash table. Naturally the hash table must already exist. */
static inline void delete_hash_table (hash_table_t htab) {
  assert (htab != NULL);
  gp_free (htab->alloc, htab->entries);
  gp_free (htab->alloc, htab);
}

static inline void _expand_hash_table (hash_table_t htab);

/* This function searches for hash table entry which contains element equal to given value or empty
   entry in which given value can be placed (if the element with given value does not exist in the
   table).  The function works in two regimes.  The first regime is used only for search.  The
   second is used for search and reservation empty entry for given value.  The table is expanded if
   occupancy (taking into accout also deleted elements) is more than 75%.  Naturally the hash table
   must already exist.  If reservation flag is TRUE then the element with given value should be
   inserted into the table entry before another call of `find_hash_table_entry'. */
static inline hash_table_entry_t *find_hash_table_entry (hash_table_t htab, hash_table_entry_t element,
                                                         bool reserve) {
  hash_table_entry_t *entry_ptr;
  hash_table_entry_t *first_deleted_entry_ptr;
  uint64_t hash_value;

  assert (htab != NULL);
  if (htab->size / 2 <= htab->number_of_elements) _expand_hash_table (htab);
  hash_value = (*htab->hash_function) (element);
  if (hash_value == 0) hash_value++;
  uint64_t mask = htab->size - 1, peterb = hash_value, ind = hash_value & mask;
  htab->searches++;
  first_deleted_entry_ptr = NULL;
  for (;; htab->collisions++) {
    entry_ptr = htab->entries + ind;
    if (*entry_ptr == EMPTY_ENTRY) {
      if (reserve) {
        htab->number_of_elements++;
        if (first_deleted_entry_ptr != NULL) {
          entry_ptr = first_deleted_entry_ptr;
          *entry_ptr = EMPTY_ENTRY;
        }
      }
      break;
    } else if (*entry_ptr != DELETED_ENTRY) {
      if ((*htab->eq_function) (*entry_ptr, element)) break;
    } else if (first_deleted_entry_ptr == NULL)
      first_deleted_entry_ptr = entry_ptr;
    peterb >>= 11;
    ind = (5 * ind + peterb + 1) & mask;
  }
  return entry_ptr;
}

/* The function changes size of memory allocated for the entries and repeatedly inserts the table
   elements.  The occupancy of the table after the call will be about 50%.  Naturally the hash table
   must already exist.  Remember also that the place of the table entries is changed. */
static inline void _expand_hash_table (hash_table_t htab) {
  hash_table_t new_htab;
  hash_table_entry_t *entry_ptr;
  hash_table_entry_t *new_entry_ptr;

  assert (htab != NULL);
  new_htab
    = create_hash_table (htab->alloc, htab->number_of_elements * 3, htab->hash_function, htab->eq_function);
  for (entry_ptr = htab->entries; entry_ptr < htab->entries + htab->size; entry_ptr++)
    if (*entry_ptr != EMPTY_ENTRY && *entry_ptr != DELETED_ENTRY) {
      new_entry_ptr = find_hash_table_entry (new_htab, *entry_ptr, true);
      assert (*new_entry_ptr == EMPTY_ENTRY);
      *new_entry_ptr = (*entry_ptr);
    }
  gp_free (htab->alloc, htab->entries);
  *htab = (*new_htab);
  gp_free (new_htab->alloc, new_htab);
}

/* Delete element with given value from hash table. The hash table entry value will be
   `DELETED_ENTRY' after the function call.  Naturally the hash table must already exist. Hash table
   entry for given value should be not empty (or deleted). */
static inline void remove_element_from_hash_table_entry (hash_table_t htab, hash_table_entry_t element) {
  hash_table_entry_t *entry_ptr;

  assert (htab != NULL);
  entry_ptr = find_hash_table_entry (htab, element, false);
  assert (*entry_ptr != EMPTY_ENTRY && *entry_ptr != DELETED_ENTRY);
  *entry_ptr = DELETED_ENTRY;
  htab->number_of_deleted_elements++;
}

/* Return current size of given hash table. */
static inline size_t hash_table_size (hash_table_t htab) {
  assert (htab != NULL);
  return htab->size;
}

/* Return current number of elements in given hash table. */
static inline size_t hash_table_elements_number (hash_table_t htab) {
  assert (htab != NULL);
  return htab->number_of_elements - htab->number_of_deleted_elements;
}

#endif /* #ifndef __HASH_TABLE__ */
