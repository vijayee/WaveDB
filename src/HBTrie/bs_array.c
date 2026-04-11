//
// Created by victor on 3/11/26
//

#include "bs_array.h"
#include "bnode.h"
#include "chunk.h"
#include "../Util/allocator.h"
#include <string.h>

void bs_array_init(bs_array_t* arr) {
  arr->entries = NULL;
  arr->count = 0;
  arr->capacity = 0;
}

bs_array_t* bs_array_create(size_t initial_capacity) {
  bs_array_t* arr = get_clear_memory(sizeof(bs_array_t));
  if (initial_capacity > 0) {
    arr->entries = get_clear_memory(sizeof(bnode_entry_t) * initial_capacity);
    arr->capacity = initial_capacity;
  }
  return arr;
}

void bs_array_destroy(bs_array_t* arr) {
  if (arr == NULL) return;
  if (arr->entries != NULL) {
    free(arr->entries);
  }
  free(arr);
}

// Compare entry key to a chunk using inline key comparison
static int bs_array_entry_cmp(bnode_entry_t* entry, chunk_t* key) {
  if (entry->key_len > 0 && entry->key_len <= BNODE_INLINE_KEY_SIZE) {
    return inline_key_compare(entry->key_data, entry->key_len, key);
  }
  return chunk_compare(entry->key, key);
}

// Binary search helper - returns index where key would be inserted
static size_t bs_array_search(bs_array_t* arr, chunk_t* key) {
  if (arr->count == 0) return 0;

  size_t lo = 0;
  size_t hi = arr->count;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int result = bs_array_entry_cmp(&arr->entries[mid], key);

    if (result < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  return lo;
}

bnode_entry_t* bs_array_find(bs_array_t* arr, chunk_t* key, size_t* out_index) {
  if (arr == NULL || key == NULL) {
    if (out_index) *out_index = 0;
    return NULL;
  }

  size_t index = bs_array_search(arr, key);

  if (out_index) *out_index = index;

  // Check if found
  if (index < arr->count) {
    int result = bs_array_entry_cmp(&arr->entries[index], key);
    if (result == 0) {
      return &arr->entries[index];
    }
  }

  return NULL;
}

int bs_array_insert(bs_array_t* arr, bnode_entry_t* entry) {
  if (arr == NULL || entry == NULL) return -1;

  // Find insertion point using entry key
  chunk_t* key = bnode_entry_get_key(entry);
  size_t index = bs_array_search(arr, key);

  // Grow array if needed
  if (arr->count >= arr->capacity) {
    size_t new_capacity = arr->capacity == 0 ? 8 : arr->capacity * 2;
    bnode_entry_t* new_entries = realloc(arr->entries, sizeof(bnode_entry_t) * new_capacity);
    if (new_entries == NULL) return -1;
    arr->entries = new_entries;
    arr->capacity = new_capacity;
  }

  // Shift entries to make room
  if (index < arr->count) {
    memmove(&arr->entries[index + 1], &arr->entries[index],
            sizeof(bnode_entry_t) * (arr->count - index));
  }

  // Insert entry
  arr->entries[index] = *entry;
  arr->count++;

  return 0;
}

bnode_entry_t* bs_array_remove_at(bs_array_t* arr, size_t index) {
  if (arr == NULL || index >= arr->count) return NULL;

  // Static to allow returning pointer
  static bnode_entry_t removed;
  removed = arr->entries[index];

  // Shift entries
  if (index < arr->count - 1) {
    memmove(&arr->entries[index], &arr->entries[index + 1],
            sizeof(bnode_entry_t) * (arr->count - index - 1));
  }

  arr->count--;

  return &removed;
}

bnode_entry_t* bs_array_get(bs_array_t* arr, size_t index) {
  if (arr == NULL || index >= arr->count) return NULL;
  return &arr->entries[index];
}

size_t bs_array_count(bs_array_t* arr) {
  if (arr == NULL) return 0;
  return arr->count;
}

int bs_array_is_empty(bs_array_t* arr) {
  return arr == NULL || arr->count == 0;
}

bnode_entry_t* bs_array_find_first(bs_array_t* arr, chunk_t* key) {
  if (arr == NULL || key == NULL) return NULL;

  size_t index;
  bnode_entry_t* found = bs_array_find(arr, key, &index);
  if (found != NULL) return found;

  // If not found exactly, return entry at insertion point
  if (index < arr->count) {
    return &arr->entries[index];
  }

  return NULL;
}

bnode_entry_t* bs_array_find_last(bs_array_t* arr, chunk_t* key) {
  if (arr == NULL || key == NULL) return NULL;

  size_t index;
  bnode_entry_t* found = bs_array_find(arr, key, &index);
  if (found != NULL) {
    // Find last matching entry (in case of duplicates)
    while (index + 1 < arr->count) {
      int result = bs_array_entry_cmp(&arr->entries[index + 1], key);
      if (result != 0) break;
      index++;
    }
    return &arr->entries[index];
  }

  // If not found exactly, return entry before insertion point
  if (index > 0) {
    return &arr->entries[index - 1];
  }

  return NULL;
}