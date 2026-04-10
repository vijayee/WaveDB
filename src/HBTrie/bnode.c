//
// Created by victor on 3/11/26.
//

#include "bnode.h"
#include <stdatomic.h>
#include "bs_array.h"
#include "chunk.h"
#include "identifier.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <string.h>

#define DEFAULT_NODE_SIZE 4096

bnode_t* bnode_create(uint32_t node_size) {
  return bnode_create_with_level(node_size, 1);
}

bnode_t* bnode_create_with_level(uint32_t node_size, uint16_t level) {
  if (node_size == 0) {
    node_size = DEFAULT_NODE_SIZE;
  }

  bnode_t* node = get_clear_memory(sizeof(bnode_t));
  node->node_size = node_size;
  node->level = level;
  vec_init(&node->entries);

  refcounter_init((refcounter_t*)node);
  atomic_init(&node->seq, 0);
  platform_lock_init(&node->write_lock);

  return node;
}

void bnode_destroy(bnode_t* node) {
  if (node == NULL) return;

  refcounter_dereference((refcounter_t*)node);
  if (refcounter_count((refcounter_t*)node) == 0) {
    platform_lock_destroy(&node->write_lock);

    // Free all entries
    for (int i = 0; i < node->entries.length; i++) {
      bnode_entry_t* entry = &node->entries.data[i];
      if (entry->key != NULL) {
        chunk_destroy(entry->key);
      }
      if (entry->has_value) {
        if (entry->has_versions && entry->versions != NULL) {
          // MVCC: destroy version chain
          version_entry_t* current = entry->versions;
          while (current != NULL) {
            version_entry_t* next = current->next;
            version_entry_destroy(current);
            current = next;
          }
        } else if (entry->value != NULL) {
          // Legacy: single value
          identifier_destroy(entry->value);
        }
        // Free path chunk counts
        if (entry->path_chunk_counts.data != NULL) {
          vec_deinit(&entry->path_chunk_counts);
        }
      } else if (entry->is_bnode_child && entry->child_bnode != NULL) {
        // Internal B+tree child - do NOT destroy here (handled by bnode_destroy_tree)
        // bnode_destroy only frees entry resources, not child bnode subtrees
      } else if (entry->child != NULL) {
        // Child hbtrie node should be destroyed separately
        // (reference counting handles this)
      }
    }

    vec_deinit(&node->entries);
    free(node);
  }
}

void bnode_destroy_tree(bnode_t* root) {
  if (root == NULL) return;

  // For internal nodes, recursively destroy child bnodes first
  if (root->level > 1) {
    for (int i = 0; i < root->entries.length; i++) {
      bnode_entry_t* entry = &root->entries.data[i];
      if (entry->is_bnode_child && entry->child_bnode != NULL) {
        bnode_destroy_tree(entry->child_bnode);
        entry->child_bnode = NULL;
      }
    }
  }

  // Now destroy this node (frees keys, values, version chains)
  bnode_destroy(root);
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

bnode_t* bnode_descend(bnode_t* root, chunk_t* key) {
  if (root == NULL || key == NULL) return NULL;

  bnode_t* current = root;

  while (current->level > 1) {
    size_t index;
    bnode_entry_t* entry = bnode_find(current, key, &index);

    if (entry != NULL && entry->is_bnode_child && entry->child_bnode != NULL) {
      // Exact match found - follow this child
      current = entry->child_bnode;
    } else {
      // No exact match - find greatest key <= search key
      // bnode_search returns the insertion point, so index-1 is the
      // rightmost entry with key < search key
      if (index == 0) {
        // Key is smaller than all entries - follow leftmost child
        bnode_entry_t* first = &current->entries.data[0];
        if (first->is_bnode_child && first->child_bnode != NULL) {
          current = first->child_bnode;
        } else {
          break;  // Should not happen in a valid internal node
        }
      } else {
        bnode_entry_t* prev = &current->entries.data[index - 1];
        if (prev->is_bnode_child && prev->child_bnode != NULL) {
          current = prev->child_bnode;
        } else {
          break;  // Should not happen in a valid internal node
        }
      }
    }
  }

  return current;
}

bnode_entry_t* bnode_find_leaf(bnode_t* root, chunk_t* key, size_t* out_index) {
  if (root == NULL || key == NULL) {
    if (out_index) *out_index = 0;
    return NULL;
  }

  // Descend to leaf through internal nodes
  bnode_t* leaf = bnode_descend(root, key);

  // Exact match in the leaf
  return bnode_find(leaf, key, out_index);
}

int bnode_insert_bnode_child(bnode_t* parent, chunk_t* key, bnode_t* child) {
  if (parent == NULL || key == NULL || child == NULL) {
    return -1;
  }

  bnode_entry_t entry = {0};
  entry.key = chunk_share(key);
  entry.child_bnode = child;
  entry.has_value = 0;
  entry.is_bnode_child = 1;

  return bnode_insert(parent, &entry);
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

int bnode_tree_is_empty(bnode_t* root) {
  if (root == NULL) return 1;

  if (root->level == 1) {
    // Leaf bnode - check directly
    return root->entries.length == 0;
  }

  // Internal bnode - check if any child subtree has entries
  for (int i = 0; i < root->entries.length; i++) {
    bnode_entry_t* entry = &root->entries.data[i];
    if (entry->is_bnode_child && entry->child_bnode != NULL) {
      if (!bnode_tree_is_empty(entry->child_bnode)) {
        return 0;  // Found a non-empty subtree
      }
    }
  }

  return 1;  // All subtrees are empty
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
  // Need at least 4 entries to split (each split node gets >= 2 entries)
  if (node->entries.length < 4) return 0;
  return bnode_size(node, chunk_size) > node->node_size;
}

int bnode_split(bnode_t* node, bnode_t** right_out, chunk_t** split_key) {
  if (node == NULL || right_out == NULL || split_key == NULL) {
    return -1;
  }

  // Minimum 4 entries required for valid split (each side gets >= 2 entries)
  if (node->entries.length < 4) {
    return -1;
  }

  // Split at midpoint
  size_t mid = (size_t)node->entries.length / 2;

  // The split_key is the separator that goes to parent
  // In B+tree: left has [0, mid), right has [mid, end)
  // The key at 'mid' becomes the separator in parent (and first entry of right)
  bnode_entry_t* mid_entry = &node->entries.data[mid];

  // COPY the split key (it will still be used in right node)
  *split_key = chunk_create(chunk_data_const(mid_entry->key), mid_entry->key->data->size);
  if (*split_key == NULL) return -1;

  // Create right node (inherit level from parent)
  bnode_t* right = bnode_create_with_level(node->node_size, node->level);
  if (right == NULL) {
    chunk_destroy(*split_key);
    *split_key = NULL;
    return -1;
  }

  // Move entries from mid to end to right node
  // The entry at 'mid' becomes the first entry of right node
  for (size_t i = mid; i < (size_t)node->entries.length; i++) {
    bnode_entry_t entry = node->entries.data[i];
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

chunk_t* bnode_get_min_key(bnode_t* node) {
  if (node == NULL || node->entries.length == 0) {
    return NULL;
  }
  return node->entries.data[0].key;
}

int bnode_insert_child(bnode_t* parent, chunk_t* key, struct hbtrie_node_t* child) {
  if (parent == NULL || key == NULL || child == NULL) {
    return -1;
  }

  bnode_entry_t entry = {0};
  entry.key = chunk_share(key);  // Share the key
  entry.child = child;
  entry.has_value = 0;

  return bnode_insert(parent, &entry);
}

// ============================================================================
// MVCC Version Chain Functions
// ============================================================================

version_entry_t* version_entry_create(transaction_id_t txn_id,
                                       identifier_t* value,
                                       uint8_t is_deleted) {
  version_entry_t* entry = get_clear_memory(sizeof(version_entry_t));
  if (entry == NULL) return NULL;

  entry->txn_id = txn_id;
  entry->value = value;  // Takes ownership of reference
  entry->is_deleted = is_deleted;
  entry->next = NULL;
  entry->prev = NULL;

  refcounter_init((refcounter_t*)entry);
  return entry;
}

void version_entry_destroy(version_entry_t* entry) {
  if (entry == NULL) return;

  refcounter_dereference((refcounter_t*)entry);
  if (refcounter_count((refcounter_t*)entry) == 0) {
    // Free value if present
    if (entry->value != NULL) {
      identifier_destroy(entry->value);
    }

    // Detach from chain
    if (entry->next != NULL) {
      entry->next->prev = entry->prev;
    }
    if (entry->prev != NULL) {
      entry->prev->next = entry->next;
    }

    free(entry);
  }
}

version_entry_t* version_entry_find_visible(version_entry_t* versions,
                                             transaction_id_t read_txn_id) {
  if (versions == NULL) return NULL;

  // FAST PATH: Most reads want the latest committed version
  // Check if the newest version is visible (common case ~90%+ hit rate)
  if (transaction_id_compare(&versions->txn_id, &read_txn_id) <= 0) {
    // Newest version is visible
    if (!versions->is_deleted) {
      log_info("MVCC Visibility: FAST PATH hit for read_txn_id=%lu.%09lu.%lu",
              read_txn_id.time, read_txn_id.nanos, read_txn_id.count);
      return versions;  // Fast path hit
    }
    // Newest version is a deletion visible to us
    log_info("MVCC Visibility: FAST PATH deleted for read_txn_id=%lu.%09lu.%lu",
            read_txn_id.time, read_txn_id.nanos, read_txn_id.count);
    return NULL;
  }

  // SLOW PATH: Walk the chain (newest first)
  log_info("MVCC Visibility: SLOW PATH for read_txn_id=%lu.%09lu.%lu",
          read_txn_id.time, read_txn_id.nanos, read_txn_id.count);

  version_entry_t* current = versions->next;  // Skip the head we already checked

  while (current != NULL) {
    log_info("  Checking version: txn_id=%lu.%09lu.%lu, compare=%d",
            current->txn_id.time, current->txn_id.nanos, current->txn_id.count,
            transaction_id_compare(&current->txn_id, &read_txn_id));

    // Check if this version is visible
    if (transaction_id_compare(&current->txn_id, &read_txn_id) <= 0) {
      // txn_id <= read_txn_id, so this version is visible
      log_info("  -> Version IS visible");
      if (!current->is_deleted) {
        return current;  // Found a visible, non-deleted version
      }
      // Deleted version - no visible version exists
      log_info("  -> Version is deleted, returning NULL");
      return NULL;
    }
    log_info("  -> Version NOT visible, trying next");
    current = current->next;  // Try older version
  }

  // No visible version found
  log_info("  -> No visible version found");
  return NULL;
}

int version_entry_add(version_entry_t** versions,
                      transaction_id_t txn_id,
                      identifier_t* value,
                      uint8_t is_deleted) {
  if (versions == NULL) return -1;

  // Create new version
  version_entry_t* new_version = version_entry_create(txn_id, value, is_deleted);
  if (new_version == NULL) return -1;

  // Insert at front of chain (newest first)
  if (*versions != NULL) {
    new_version->next = *versions;
    (*versions)->prev = new_version;
  }

  *versions = new_version;
  return 0;
}

size_t version_entry_gc(version_entry_t** versions, transaction_id_t min_active_txn_id) {
  if (versions == NULL || *versions == NULL) return 0;

  size_t removed_count = 0;
  version_entry_t* current = *versions;

  // Find the newest version (head of chain)
  // We always keep the newest committed version
  while (current->next != NULL) {
    current = current->next;
  }

  // Now current is the oldest version
  // Remove old versions starting from oldest
  while (current != NULL && current != *versions) {
    // Check if this version is older than min_active_txn_id
    if (transaction_id_compare(&current->txn_id, &min_active_txn_id) < 0) {
      version_entry_t* to_remove = current;
      current = current->prev;

      // Detach from chain
      if (to_remove->prev != NULL) {
        to_remove->prev->next = to_remove->next;
      }
      if (to_remove->next != NULL) {
        to_remove->next->prev = to_remove->prev;
      }

      version_entry_destroy(to_remove);
      removed_count++;
    } else {
      break;  // Versions are newest-first, so stop when we hit a visible one
    }
  }

  return removed_count;
}

// ============================================================================
// Path Chunk Counts Functions
// ============================================================================

int bnode_entry_set_path_chunk_counts(bnode_entry_t* entry,
                                       const size_t* counts,
                                       size_t count) {
  if (entry == NULL) return -1;
  if (!entry->has_value) return -1;  // Only valid for leaf entries

  // Initialize the vector if needed
  if (entry->path_chunk_counts.data == NULL) {
    vec_init(&entry->path_chunk_counts);
  }

  // Reserve exact capacity to avoid overallocation
  if (vec_reserve(&entry->path_chunk_counts, (int)count) != 0) {
    return -1;
  }

  // Copy the counts
  vec_clear(&entry->path_chunk_counts);
  for (size_t i = 0; i < count; i++) {
    vec_push(&entry->path_chunk_counts, counts[i]);
  }

  return 0;
}

const size_t* bnode_entry_get_path_chunk_counts(const bnode_entry_t* entry,
                                                size_t* out_count) {
  if (entry == NULL || !entry->has_value) {
    if (out_count) *out_count = 0;
    return NULL;
  }

  if (entry->path_chunk_counts.data == NULL || entry->path_chunk_counts.length == 0) {
    // Legacy entry - treat as single identifier
    if (out_count) *out_count = 0;
    return NULL;
  }

  if (out_count) {
    *out_count = (size_t)entry->path_chunk_counts.length;
  }

  return entry->path_chunk_counts.data;
}