//
// Created by victor on 3/11/26.
//

#include "bnode.h"
#include "../Storage/sections.h"
#include <stdatomic.h>
#include "bs_array.h"
#include "chunk.h"
#include "identifier.h"
#include "../Util/allocator.h"
#include "../Util/memory_pool.h"
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

  bnode_t* node = memory_pool_alloc(sizeof(bnode_t));
  if (node == NULL) return NULL;

  // Zero-initialize the struct, then properly init atomic/lock fields
  memset(node, 0, sizeof(bnode_t));

  // Initialize fields that need proper initialization (not just zero)
  atomic_init(&node->level, level);
  node->node_size = node_size;
  vec_init(&node->entries);
  atomic_init(&node->seq, 0);
  platform_lock_init(&node->write_lock);
  refcounter_init((refcounter_t*)node);

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
      bnode_entry_destroy_key(entry);
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
    memory_pool_free(node, sizeof(bnode_t));
  }
}

void bnode_destroy_tree(bnode_t* root) {
  if (root == NULL) return;

  // For internal nodes, recursively destroy child bnodes first
  if (atomic_load(&root->level) > 1) {
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

// Binary search for key position using inline key comparison
static size_t bnode_search(bnode_t* node, chunk_t* key) {
  if (node->entries.length == 0) return 0;

  size_t lo = 0;
  size_t hi = (size_t)node->entries.length;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    bnode_entry_t* entry = &node->entries.data[mid];

    // Use inline key comparison for fast path (no pointer chase)
    int cmp;
    if (entry->key_len > 0 && entry->key_len <= BNODE_INLINE_KEY_SIZE) {
      cmp = inline_key_compare(entry->key_data, entry->key_len, key);
    } else {
      cmp = chunk_compare(entry->key, key);
    }

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
    // Use inline key comparison for final check
    int cmp;
    if (entry->key_len > 0 && entry->key_len <= BNODE_INLINE_KEY_SIZE) {
      cmp = inline_key_compare(entry->key_data, entry->key_len, key);
    } else {
      cmp = chunk_compare(entry->key, key);
    }
    if (cmp == 0) {
      return entry;
    }
  }

  return NULL;
}

bnode_t* bnode_descend(bnode_t* root, chunk_t* key) {
  if (root == NULL || key == NULL) return NULL;

  bnode_t* current = root;

  while (atomic_load(&current->level) > 1) {
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
  bnode_entry_set_key(&entry, key);
  entry.child_bnode = child;
  entry.has_value = 0;
  entry.is_bnode_child = 1;

  return bnode_insert(parent, &entry);
}

int bnode_insert(bnode_t* node, bnode_entry_t* entry) {
  if (node == NULL || entry == NULL) return -1;

  // Find insertion point using the entry's key
  chunk_t* key = bnode_entry_get_key(entry);
  size_t index = bnode_search(node, key);

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

  if (atomic_load(&root->level) == 1) {
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
    // Chunk: struct + inline data
    chunk_t* key = bnode_entry_get_key(entry);
    if (key != NULL) {
      size += sizeof(chunk_t) + key->size;
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
  chunk_t* mid_key = bnode_entry_get_key(mid_entry);
  *split_key = chunk_create(chunk_data_const(mid_key), mid_key->size);
  if (*split_key == NULL) return -1;

  // Create right node (inherit level from parent)
  bnode_t* right = bnode_create_with_level(node->node_size, atomic_load(&node->level));
  if (right == NULL) {
    chunk_destroy(*split_key);
    *split_key = NULL;
    return -1;
  }

  // Move entries from mid to end to right node
  // The entry at 'mid' becomes the first entry of right node
  // We must properly share key/value refcounts since both the left node's
  // hidden entries and the right node's entries will point to the same objects.
  for (size_t i = mid; i < (size_t)node->entries.length; i++) {
    bnode_entry_t* src = &node->entries.data[i];
    bnode_entry_t new_entry = {0};

    // Share the key (increment refcount)
    bnode_entry_set_key(&new_entry, bnode_entry_get_key(src));

    // Copy other fields
    new_entry.has_value = src->has_value;
    new_entry.has_versions = src->has_versions;
    new_entry.is_bnode_child = src->is_bnode_child;
    new_entry.value_txn_id = src->value_txn_id;

    if (src->has_value) {
      if (src->has_versions && src->versions != NULL) {
        // MVCC version chain - transfer pointer (version chain owns its refs)
        new_entry.versions = src->versions;
      } else if (src->value != NULL) {
        // Legacy value - share reference (increment refcount)
        new_entry.value = (identifier_t*)refcounter_reference((refcounter_t*)src->value);
      }
      // Copy trie_child if present (entry has both value and child)
      new_entry.trie_child = src->trie_child;
    } else if (src->is_bnode_child && src->child_bnode != NULL) {
      new_entry.child_bnode = src->child_bnode;
    } else if (src->child != NULL) {
      new_entry.child = src->child;
    }

    // Copy path chunk counts if present
    if (src->has_value && src->path_chunk_counts.data != NULL) {
      bnode_entry_set_path_chunk_counts(&new_entry,
          src->path_chunk_counts.data,
          (size_t)src->path_chunk_counts.length);
    }

    // Copy storage location fields
    new_entry.child_disk_offset = src->child_disk_offset;

    // Insert into right node
    bnode_insert(right, &new_entry);
  }

  // Destroy keys and values of the hidden entries in the left node
  // (they're now owned by the right node's copies, which hold proper references)
  for (size_t i = mid; i < (size_t)node->entries.length; i++) {
    bnode_entry_t* entry = &node->entries.data[i];
    // Destroy key (the right node's copy has its own reference via chunk_share)
    bnode_entry_destroy_key(entry);
    if (entry->has_value) {
      if (entry->has_versions && entry->versions != NULL) {
        // Version chain - clear pointer (right node now owns it)
        entry->versions = NULL;
      } else if (entry->value != NULL) {
        // Legacy value - dereference our copy (right node has its own reference)
        identifier_destroy(entry->value);
        entry->value = NULL;
      }
    }
    // Clear child pointers so bnode_destroy won't double-free
    entry->child = NULL;
    entry->child_bnode = NULL;
    entry->trie_child = NULL;
    // Free path_chunk_counts data (right node's copy has its own allocation)
    if (entry->path_chunk_counts.data != NULL) {
      vec_deinit(&entry->path_chunk_counts);
      entry->path_chunk_counts.data = NULL;
    }
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
  // Use inline key comparison for fast path
  if (a->key_len > 0 && a->key_len <= BNODE_INLINE_KEY_SIZE) {
    return inline_key_compare(a->key_data, a->key_len, key);
  }
  return chunk_compare(a->key, key);
}

chunk_t* bnode_get_min_key(bnode_t* node) {
  if (node == NULL || node->entries.length == 0) {
    return NULL;
  }
  return bnode_entry_get_key(&node->entries.data[0]);
}

int bnode_insert_child(bnode_t* parent, chunk_t* key, struct hbtrie_node_t* child) {
  if (parent == NULL || key == NULL || child == NULL) {
    return -1;
  }

  bnode_entry_t entry = {0};
  bnode_entry_set_key(&entry, key);
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
  version_entry_t* entry = memory_pool_alloc(sizeof(version_entry_t));
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

    memory_pool_free(entry, sizeof(version_entry_t));
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

size_t version_entry_gc(version_entry_t** versions, transaction_id_t min_active_txn_id,
                         sections_t* storage) {
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

      // Deallocate section storage for the removed version's value
      if (storage != NULL && to_remove->value_section_id != 0) {
        sections_deallocate(storage, to_remove->value_section_id,
                            to_remove->value_offset, to_remove->value_data_size);
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

// ============================================================================
// Inline Key Management Functions
// ============================================================================

void bnode_entry_set_key(bnode_entry_t* entry, chunk_t* key) {
  if (entry == NULL) return;

  if (key == NULL) {
    entry->key = NULL;
    entry->key_len = 0;
    memset(entry->key_data, 0, BNODE_INLINE_KEY_SIZE);
    return;
  }

  // Always store the chunk_t* reference for non-comparison uses
  entry->key = chunk_share(key);
  entry->key_len = (uint8_t)key->size;

  // Copy key data inline if it fits
  if (key->size <= BNODE_INLINE_KEY_SIZE) {
    memcpy(entry->key_data, key->data, key->size);
    // Zero remaining bytes for clean comparison
    if (key->size < BNODE_INLINE_KEY_SIZE) {
      memset(entry->key_data + key->size, 0, BNODE_INLINE_KEY_SIZE - key->size);
    }
  } else {
    // Key too large for inline storage - will use key pointer for comparison
    memset(entry->key_data, 0, BNODE_INLINE_KEY_SIZE);
  }
}

chunk_t* bnode_entry_get_key(bnode_entry_t* entry) {
  if (entry == NULL) return NULL;
  return entry->key;
}

void bnode_entry_destroy_key(bnode_entry_t* entry) {
  if (entry == NULL) return;
  if (entry->key != NULL) {
    chunk_destroy(entry->key);
    entry->key = NULL;
  }
  entry->key_len = 0;
  memset(entry->key_data, 0, BNODE_INLINE_KEY_SIZE);
}