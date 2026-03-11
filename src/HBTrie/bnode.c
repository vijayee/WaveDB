//
// Created by victor on 3/11/26.
//

#include "bnode.h"
#include "bs_array.h"
#include "chunk.h"
#include "identifier.h"
#include "../Util/allocator.h"
#include <string.h>

#define DEFAULT_NODE_SIZE 4096

bnode_t* bnode_create(uint32_t node_size) {
  if (node_size == 0) {
    node_size = DEFAULT_NODE_SIZE;
  }

  bnode_t* node = get_clear_memory(sizeof(bnode_t));
  node->node_size = node_size;
  vec_init(&node->entries);

  refcounter_init((refcounter_t*)node);
  platform_lock_init(&node->lock);

  return node;
}

void bnode_destroy(bnode_t* node) {
  if (node == NULL) return;

  refcounter_dereference((refcounter_t*)node);
  if (refcounter_count((refcounter_t*)node) == 0) {
    platform_lock_destroy(&node->lock);

    // Free all entries
    for (int i = 0; i < node->entries.length; i++) {
      bnode_entry_t* entry = &node->entries.data[i];
      if (entry->key != NULL) {
        chunk_destroy(entry->key);
      }
      if (entry->has_value && entry->value != NULL) {
        identifier_destroy(entry->value);
      } else if (!entry->has_value && entry->child != NULL) {
        // Child node should be destroyed separately
        // (reference counting handles this)
      }
    }

    vec_deinit(&node->entries);
    free(node);
  }
}

// Binary search for key position
static size_t bnode_search(bnode_t* node, chunk_t* key) {
  if (node->entries.length == 0) return 0;

  size_t lo = 0;
  size_t hi = (size_t)node->entries.length;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    bnode_entry_t* entry = &node->entries.data[mid];
    int cmp = chunk_compare(entry->key, key);

    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  return lo;
}

bnode_entry_t* bnode_find(bnode_t* node, chunk_t* key, size_t* out_index) {
  if (node == NULL || key == NULL) {
    if (out_index) *out_index = 0;
    return NULL;
  }

  size_t index = bnode_search(node, key);

  if (out_index) *out_index = index;

  // Check if found
  if (index < (size_t)node->entries.length) {
    bnode_entry_t* entry = &node->entries.data[index];
    if (chunk_compare(entry->key, key) == 0) {
      return entry;
    }
  }

  return NULL;
}

int bnode_insert(bnode_t* node, bnode_entry_t* entry) {
  if (node == NULL || entry == NULL) return -1;

  // Find insertion point
  size_t index = bnode_search(node, entry->key);

  // Reference the key
  // Note: entry->key should already be allocated; we take ownership

  // Insert into vector
  return vec_insert(&node->entries, (int)index, *entry);
}

bnode_entry_t bnode_remove(bnode_t* node, chunk_t* key) {
  bnode_entry_t removed = {0};

  size_t index;
  bnode_entry_t* found = bnode_find(node, key, &index);
  if (found == NULL) {
    return removed;
  }

  return bnode_remove_at(node, index);
}

bnode_entry_t bnode_remove_at(bnode_t* node, size_t index) {
  bnode_entry_t removed = {0};

  if (node == NULL || index >= (size_t)node->entries.length) {
    return removed;
  }

  removed = node->entries.data[index];
  vec_splice(&node->entries, (int)index, 1);

  return removed;
}

bnode_entry_t* bnode_get(bnode_t* node, size_t index) {
  if (node == NULL || index >= (size_t)node->entries.length) {
    return NULL;
  }
  return &node->entries.data[index];
}

size_t bnode_count(bnode_t* node) {
  if (node == NULL) return 0;
  return (size_t)node->entries.length;
}

int bnode_is_empty(bnode_t* node) {
  return node == NULL || node->entries.length == 0;
}

size_t bnode_size(bnode_t* node, uint8_t chunk_size) {
  if (node == NULL) return 0;

  // Base size: struct overhead + entries vector
  size_t size = sizeof(bnode_t);

  // Each entry: struct + chunk buffer
  for (int i = 0; i < node->entries.length; i++) {
    bnode_entry_t* entry = &node->entries.data[i];
    size += sizeof(bnode_entry_t);
    // Chunk: struct + buffer data
    if (entry->key != NULL && entry->key->data != NULL) {
      size += sizeof(chunk_t) + chunk_size;
    }
  }

  return size;
}

int bnode_needs_split(bnode_t* node, uint8_t chunk_size) {
  if (node == NULL) return 0;
  return bnode_size(node, chunk_size) > node->node_size && node->entries.length > 1;
}

int bnode_split(bnode_t* node, bnode_t** right_out, chunk_t** split_key) {
  if (node == NULL || right_out == NULL || split_key == NULL) {
    return -1;
  }

  if (node->entries.length < 2) {
    return -1;  // Can't split with less than 2 entries
  }

  // Split at midpoint
  size_t mid = (size_t)node->entries.length / 2;

  // Create right node
  bnode_t* right = bnode_create(node->node_size);
  if (right == NULL) return -1;

  // Get the split key (key at midpoint - this will be promoted)
  // In B+tree, the key at mid goes to parent, and right node starts at mid
  bnode_entry_t* mid_entry = &node->entries.data[mid];
  *split_key = mid_entry->key;

  // Move entries from mid to end to right node
  for (int i = (int)mid; i < node->entries.length; i++) {
    bnode_entry_t entry = node->entries.data[i];
    // Share the key (don't take ownership - parent will manage it)
    bnode_insert(right, &entry);
  }

  // Truncate left node to entries before mid
  node->entries.length = (int)mid;

  *right_out = right;
  return 0;
}

int bnode_entry_compare(bnode_entry_t* a, chunk_t* key) {
  if (a == NULL && key == NULL) return 0;
  if (a == NULL) return -1;
  if (key == NULL) return 1;
  return chunk_compare(a->key, key);
}