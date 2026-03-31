//
// Created by victor on 3/11/26.
//

#include "hbtrie.h"
#include "mvcc.h"
#include "bnode.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <cbor.h>
#include <string.h>

// Default: enough space for ~64 entries per node
// Each entry is roughly: sizeof(bnode_entry_t) + chunk_size for key buffer
#define DEFAULT_ENTRIES_PER_NODE 64

hbtrie_t* hbtrie_create(uint8_t chunk_size, uint32_t btree_node_size) {
  if (chunk_size == 0) {
    chunk_size = DEFAULT_CHUNK_SIZE;
  }
  if (btree_node_size == 0) {
    // Calculate default based on chunk size
    // Each entry needs: chunk key buffer + bnode_entry_t overhead (~24 bytes)
    btree_node_size = DEFAULT_ENTRIES_PER_NODE * (chunk_size + 24);
  }

  // Cap btree_node_size at maximum entries possible for this chunk size
  // Max unique chunks = 2^(chunk_size * 8)
  // For chunk_size >= 2, this is huge, so we don't cap those
  // For chunk_size = 1, max is 256 unique values
  if (chunk_size == 1) {
    uint32_t max_node_size = 256 * (chunk_size + 24);
    if (btree_node_size > max_node_size) {
      btree_node_size = max_node_size;
    }
  }

  hbtrie_t* trie = get_clear_memory(sizeof(hbtrie_t));
  trie->chunk_size = chunk_size;
  trie->btree_node_size = btree_node_size;

  // Create empty root node
  trie->root = hbtrie_node_create(btree_node_size);
  if (trie->root == NULL) {
    free(trie);
    return NULL;
  }

  refcounter_init((refcounter_t*)trie);
  platform_lock_init(&trie->lock);

  return trie;
}

void hbtrie_destroy(hbtrie_t* trie) {
  if (trie == NULL) return;

  refcounter_dereference((refcounter_t*)trie);
  if (refcounter_count((refcounter_t*)trie) == 0) {
    if (trie->root != NULL) {
      hbtrie_node_destroy(trie->root);
    }
    platform_lock_destroy(&trie->lock);
    refcounter_destroy_lock((refcounter_t*)trie);
    free(trie);
  }
}

hbtrie_node_t* hbtrie_node_create(uint32_t btree_node_size) {
  hbtrie_node_t* node = get_clear_memory(sizeof(hbtrie_node_t));

  node->btree = bnode_create(btree_node_size);
  if (node->btree == NULL) {
    free(node);
    return NULL;
  }

  // Initialize storage tracking (in-memory by default)
  node->storage = NULL;          // NULL = in-memory only
  node->is_loaded = 1;           // Newly created nodes are in memory
  node->is_dirty = 0;            // Not modified yet

  platform_lock_init(&node->lock);
  refcounter_init((refcounter_t*)node);

  return node;
}

void hbtrie_node_destroy(hbtrie_node_t* node) {
  if (node == NULL) return;

  refcounter_dereference((refcounter_t*)node);
  if (refcounter_count((refcounter_t*)node) == 0) {
    // Iterative destroy: collect all nodes first, then destroy bottom-up
    // Use a dynamic vector for the stack
    vec_t(hbtrie_node_t*) nodes;
    vec_init(&nodes);

    // Push root node
    vec_push(&nodes, node);

    // Collect all child nodes iteratively
    for (int i = 0; i < nodes.length; i++) {
      hbtrie_node_t* current = nodes.data[i];
      if (current->btree != NULL) {
        for (size_t j = 0; j < bnode_count(current->btree); j++) {
          bnode_entry_t* entry = bnode_get(current->btree, j);
          if (entry != NULL && !entry->has_value && entry->child != NULL) {
            vec_push(&nodes, entry->child);
          }
        }
      }
    }

    // Destroy nodes in reverse order (bottom-up: children before parents)
    for (int i = nodes.length - 1; i >= 0; i--) {
      hbtrie_node_t* current = nodes.data[i];
      if (current->btree != NULL) {
        bnode_destroy(current->btree);
      }
      platform_lock_destroy(&current->lock);
      refcounter_destroy_lock((refcounter_t*)current);
      free(current);
    }

    vec_deinit(&nodes);
  }
}

/**
 * Split an hbtrie_node when its bnode exceeds the size limit.
 * Creates a new intermediate node with two children (left and right halves).
 *
 * @param node            Node whose bnode needs splitting
 * @param btree_node_size Max bnode size for new nodes
 * @param chunk_size      Chunk size for the trie
 * @return New intermediate node, or NULL on failure. Original node becomes left child.
 */
static hbtrie_node_t* hbtrie_node_split(hbtrie_node_t* node, uint32_t btree_node_size, uint8_t chunk_size) {
  if (node == NULL || node->btree == NULL) return NULL;

  chunk_t* split_key = NULL;
  bnode_t* right_bnode = NULL;

  if (bnode_split(node->btree, &right_bnode, &split_key) != 0) {
    return NULL;
  }

  // Create right child hbtrie_node
  hbtrie_node_t* right_child = hbtrie_node_create(btree_node_size);
  if (right_child == NULL) {
    bnode_destroy(right_bnode);
    return NULL;
  }
  right_child->btree = right_bnode;

  // Create new parent node
  hbtrie_node_t* parent = hbtrie_node_create(btree_node_size);
  if (parent == NULL) {
    hbtrie_node_destroy(right_child);
    return NULL;
  }

  // Add left child entry (first key from left bnode)
  bnode_entry_t* left_first = bnode_get(node->btree, 0);
  if (left_first != NULL) {
    bnode_entry_t left_entry = {0};
    left_entry.key = chunk_share(left_first->key);
    left_entry.has_value = 0;
    left_entry.child = node;
    bnode_insert(parent->btree, &left_entry);
  }

  // Add right child entry (first key from right bnode)
  bnode_entry_t* right_first = bnode_get(right_bnode, 0);
  if (right_first != NULL) {
    bnode_entry_t right_entry = {0};
    right_entry.key = chunk_share(right_first->key);
    right_entry.has_value = 0;
    right_entry.child = right_child;
    bnode_insert(parent->btree, &right_entry);
  }

  return parent;
}

int hbtrie_insert(hbtrie_t* trie, path_t* path, identifier_t* value) {
  if (trie == NULL || path == NULL || value == NULL) {
    return -1;
  }

  platform_lock(&trie->lock);

  // Track path for potential splitting
  typedef struct {
    hbtrie_node_t* node;
    bnode_entry_t* entry;
    chunk_t* chunk;
  } insert_path_item_t;
  vec_t(insert_path_item_t) insert_path;
  vec_init(&insert_path);

  hbtrie_node_t* current = trie->root;
  size_t path_len_ids = path_length(path);

  if (path_len_ids == 0) {
    // Empty path - can't insert
    vec_deinit(&insert_path);
    platform_unlock(&trie->lock);
    return -1;
  }

  // Traverse through each identifier in the path
  for (size_t i = 0; i < path_len_ids; i++) {
    identifier_t* identifier = path_get(path, i);
    if (identifier == NULL) {
      vec_deinit(&insert_path);
      platform_unlock(&trie->lock);
      return -1;
    }

    size_t nchunk = identifier_chunk_count(identifier);

    // Traverse through chunks of this identifier
    for (size_t j = 0; j < nchunk; j++) {
      chunk_t* chunk = identifier_get_chunk(identifier, j);
      if (chunk == NULL) {
        vec_deinit(&insert_path);
        platform_unlock(&trie->lock);
        return -1;
      }

      size_t index;
      bnode_entry_t* entry = bnode_find(current->btree, chunk, &index);

      int is_last_chunk = (j == nchunk - 1);
      int is_last_identifier = (i == path_len_ids - 1);

      if (is_last_chunk && is_last_identifier) {
        // Final position - store the value
        if (entry != NULL) {
          // Update existing entry
          if (entry->has_value && entry->value != NULL) {
            identifier_destroy(entry->value);
          }
          entry->has_value = 1;
          entry->value = (identifier_t*)refcounter_reference((refcounter_t*)value);
        } else {
          // Create new entry with value
          bnode_entry_t new_entry = {0};
          new_entry.key = chunk_create(chunk_data_const(chunk), trie->chunk_size);
          new_entry.has_value = 1;
          new_entry.value = (identifier_t*)refcounter_reference((refcounter_t*)value);
          bnode_insert(current->btree, &new_entry);
        }
      } else if (is_last_chunk) {
        // End of this identifier, need to move to next HBTrie level
        if (entry == NULL) {
          // Create new child HBTrie node
          hbtrie_node_t* child = hbtrie_node_create(trie->btree_node_size);
          if (child == NULL) {
            vec_deinit(&insert_path);
            platform_unlock(&trie->lock);
            return -1;
          }

          bnode_entry_t new_entry = {0};
          new_entry.key = chunk_create(chunk_data_const(chunk), trie->chunk_size);
          new_entry.has_value = 0;
          new_entry.child = child;
          bnode_insert(current->btree, &new_entry);

          // Track path for potential split
          insert_path_item_t path_item = {
            current,
            &current->btree->entries.data[current->btree->entries.length - 1],
            new_entry.key
          };
          vec_push(&insert_path, path_item);

          current = child;
        } else if (!entry->has_value && entry->child != NULL) {
          // Traverse to existing child
          current = entry->child;
        } else {
          // Entry has a value - need to create subtree
          vec_deinit(&insert_path);
          platform_unlock(&trie->lock);
          return -1;
        }
      } else {
        // Intermediate chunk within this identifier
        if (entry == NULL) {
          // Create intermediate node (same HBTrie level, continuing chunk chain)
          hbtrie_node_t* child = hbtrie_node_create(trie->btree_node_size);
          if (child == NULL) {
            vec_deinit(&insert_path);
            platform_unlock(&trie->lock);
            return -1;
          }

          bnode_entry_t new_entry = {0};
          new_entry.key = chunk_create(chunk_data_const(chunk), trie->chunk_size);
          new_entry.has_value = 0;
          new_entry.child = child;
          bnode_insert(current->btree, &new_entry);

          current = child;
        } else if (!entry->has_value && entry->child != NULL) {
          current = entry->child;
        } else {
          // Entry has a value - can't continue
          vec_deinit(&insert_path);
          platform_unlock(&trie->lock);
          return -1;
        }
      }
    }
  }

  // Check if root needs splitting and update root if needed
  if (bnode_needs_split(trie->root->btree, trie->chunk_size)) {
    hbtrie_node_t* new_root = hbtrie_node_split(trie->root, trie->btree_node_size, trie->chunk_size);
    if (new_root != NULL) {
      trie->root = new_root;
    }
  }

  vec_deinit(&insert_path);
  platform_unlock(&trie->lock);
  return 0;
}

identifier_t* hbtrie_find(hbtrie_t* trie, path_t* path) {
  if (trie == NULL || path == NULL) {
    return NULL;
  }

  platform_lock(&trie->lock);

  hbtrie_node_t* current = trie->root;
  size_t path_len_ids = path_length(path);

  if (path_len_ids == 0) {
    platform_unlock(&trie->lock);
    return NULL;
  }

  // Traverse through each identifier in the path
  for (size_t i = 0; i < path_len_ids; i++) {
    identifier_t* identifier = path_get(path, i);
    if (identifier == NULL) {
      platform_unlock(&trie->lock);
      return NULL;
    }

    size_t nchunk = identifier_chunk_count(identifier);

    // Traverse through chunks of this identifier
    for (size_t j = 0; j < nchunk; j++) {
      chunk_t* chunk = identifier_get_chunk(identifier, j);
      if (chunk == NULL) {
        platform_unlock(&trie->lock);
        return NULL;
      }

      size_t index;
      bnode_entry_t* entry = bnode_find(current->btree, chunk, &index);

      int is_last_chunk = (j == nchunk - 1);
      int is_last_identifier = (i == path_len_ids - 1);

      if (is_last_chunk && is_last_identifier) {
        // Final position - return the value if found
        if (entry != NULL && entry->has_value) {
          identifier_t* result = (identifier_t*)refcounter_reference((refcounter_t*)entry->value);
          platform_unlock(&trie->lock);
          return result;
        }
        platform_unlock(&trie->lock);
        return NULL;
      } else if (is_last_chunk) {
        // End of this identifier, need to move to next HBTrie level
        if (entry == NULL || entry->has_value || entry->child == NULL) {
          platform_unlock(&trie->lock);
          return NULL;
        }
        current = entry->child;
      } else {
        // Intermediate chunk within this identifier
        if (entry == NULL || entry->has_value || entry->child == NULL) {
          platform_unlock(&trie->lock);
          return NULL;
        }
        current = entry->child;
      }
    }
  }

  platform_unlock(&trie->lock);
  return NULL;
}

identifier_t* hbtrie_remove(hbtrie_t* trie, path_t* path) {
  if (trie == NULL || path == NULL) {
    return NULL;
  }

  platform_lock(&trie->lock);

  // Track path for cleanup: each entry is (parent_node, entry_in_parent)
  // We track entries, not nodes, so we can remove entries from bnodes
  typedef struct {
    hbtrie_node_t* parent_node;
    size_t entry_index;
    chunk_t* chunk;
  } remove_path_item_t;
  vec_t(remove_path_item_t) remove_path;
  vec_init(&remove_path);

  hbtrie_node_t* current = trie->root;
  size_t path_len_ids = path_length(path);

  if (path_len_ids == 0) {
    vec_deinit(&remove_path);
    platform_unlock(&trie->lock);
    return NULL;
  }

  // Traverse to find the value
  for (size_t i = 0; i < path_len_ids; i++) {
    identifier_t* identifier = path_get(path, i);
    if (identifier == NULL) {
      vec_deinit(&remove_path);
      platform_unlock(&trie->lock);
      return NULL;
    }

    size_t nchunk = identifier_chunk_count(identifier);

    for (size_t j = 0; j < nchunk; j++) {
      chunk_t* chunk = identifier_get_chunk(identifier, j);
      if (chunk == NULL) {
        vec_deinit(&remove_path);
        platform_unlock(&trie->lock);
        return NULL;
      }

      size_t index;
      bnode_entry_t* entry = bnode_find(current->btree, chunk, &index);

      int is_last_chunk = (j == nchunk - 1);
      int is_last_identifier = (i == path_len_ids - 1);

      if (is_last_chunk && is_last_identifier) {
        // Final position - remove the value
        if (entry == NULL || !entry->has_value) {
          vec_deinit(&remove_path);
          platform_unlock(&trie->lock);
          return NULL;
        }

        identifier_t* result = entry->value;

        // Remove the entry entirely from the current node
        bnode_remove_at(current->btree, index);

        // Clean up empty nodes along the path (in reverse order)
        // Start from the last tracked parent and work up
        hbtrie_node_t* cleanup_node = current;
        for (int k = (int)remove_path.length - 1; k >= 0; k--) {
          hbtrie_node_t* parent_node = remove_path.data[k].parent_node;
          size_t parent_entry_index = remove_path.data[k].entry_index;

          // If the node we're cleaning up is empty, remove it from parent
          if (bnode_is_empty(cleanup_node->btree)) {
            // Destroy the empty child node
            hbtrie_node_destroy(cleanup_node);

            // Remove the entry from parent's bnode that pointed to this child
            bnode_remove_at(parent_node->btree, parent_entry_index);

            // Move up to parent for next iteration
            cleanup_node = parent_node;
          } else {
            // Node still has entries, stop cleanup
            break;
          }
        }

        vec_deinit(&remove_path);
        platform_unlock(&trie->lock);
        return result;
      } else if (is_last_chunk) {
        // End of this identifier, need to move to next HBTrie level
        if (entry == NULL || entry->has_value || entry->child == NULL) {
          vec_deinit(&remove_path);
          platform_unlock(&trie->lock);
          return NULL;
        }

        // Track path for potential cleanup
        remove_path_item_t rp_item = {current, index, chunk};
        vec_push(&remove_path, rp_item);

        current = entry->child;
      } else {
        // Intermediate chunk within this identifier
        if (entry == NULL || entry->has_value || entry->child == NULL) {
          vec_deinit(&remove_path);
          platform_unlock(&trie->lock);
          return NULL;
        }

        remove_path_item_t rp_item = {current, index, chunk};
        vec_push(&remove_path, rp_item);

        current = entry->child;
      }
    }
  }

  vec_deinit(&remove_path);
  platform_unlock(&trie->lock);
  return NULL;
}

hbtrie_node_t* hbtrie_node_copy(hbtrie_node_t* node) {
  if (node == NULL) return NULL;

  hbtrie_node_t* copy = hbtrie_node_create(node->btree->node_size);
  if (copy == NULL) return NULL;

  // Copy each entry
  for (int i = 0; i < node->btree->entries.length; i++) {
    bnode_entry_t* entry = &node->btree->entries.data[i];
    bnode_entry_t new_entry = {0};

    new_entry.key = chunk_share(entry->key);
    new_entry.has_value = entry->has_value;

    if (entry->has_value) {
      new_entry.value = (identifier_t*)refcounter_reference((refcounter_t*)entry->value);
    } else if (entry->child != NULL) {
      new_entry.child = hbtrie_node_copy(entry->child);
    }

    bnode_insert(copy->btree, &new_entry);
  }

  return copy;
}

hbtrie_t* hbtrie_copy(hbtrie_t* trie) {
  if (trie == NULL) return NULL;

  hbtrie_t* copy = get_clear_memory(sizeof(hbtrie_t));
  if (copy == NULL) return NULL;

  copy->chunk_size = trie->chunk_size;
  copy->btree_node_size = trie->btree_node_size;

  if (trie->root != NULL) {
    copy->root = hbtrie_node_copy(trie->root);
    if (copy->root == NULL) {
      free(copy);
      return NULL;
    }
  }

  refcounter_init((refcounter_t*)copy);
  platform_lock_init(&copy->lock);

  return copy;
}

void hbtrie_cursor_init(hbtrie_cursor_t* cursor, hbtrie_t* trie, path_t* path) {
  if (cursor == NULL || trie == NULL) return;

  cursor->current = trie->root;
  cursor->identifier_index = 0;
  cursor->chunk_pos = 0;

  (void)path; // Path is for future use in more complex traversals
}

int hbtrie_cursor_next(hbtrie_cursor_t* cursor) {
  if (cursor == NULL || cursor->current == NULL) return -1;

  // For now, this is a simple placeholder
  // Full implementation would iterate through all chunks in the current node
  cursor->chunk_pos++;

  return 0;
}

int hbtrie_cursor_at_end(hbtrie_cursor_t* cursor) {
  if (cursor == NULL || cursor->current == NULL) return 1;

  // Check if we've processed all chunks
  return cursor->chunk_pos >= bnode_count(cursor->current->btree);
}

chunk_t* hbtrie_cursor_get_chunk(hbtrie_cursor_t* cursor) {
  if (cursor == NULL || cursor->current == NULL) return NULL;

  bnode_entry_t* entry = bnode_get(cursor->current->btree, cursor->chunk_pos);
  if (entry == NULL) return NULL;

  return entry->key;
}

hbtrie_node_t* hbtrie_cursor_get_node(hbtrie_cursor_t* cursor) {
  if (cursor == NULL) return NULL;
  return cursor->current;
}

// Forward declaration for recursive serialization
static cbor_item_t* hbtrie_node_to_cbor(hbtrie_node_t* node);
static hbtrie_node_t* cbor_to_hbtrie_node(cbor_item_t* item, uint32_t btree_node_size);

static cbor_item_t* hbtrie_node_to_cbor(hbtrie_node_t* node) {
  if (node == NULL) {
    return cbor_new_null();
  }

  // Create array of entries
  cbor_item_t* entries = cbor_new_definite_array((size_t)node->btree->entries.length);
  if (entries == NULL) return NULL;

  for (int i = 0; i < node->btree->entries.length; i++) {
    bnode_entry_t* entry = &node->btree->entries.data[i];
    cbor_item_t* entry_item = cbor_new_definite_array(3); // [key_bstr, has_value, value_or_child]
    if (entry_item == NULL) {
      cbor_decref(&entries);
      return NULL;
    }

    // Key as byte string
    cbor_item_t* key_bstr = cbor_build_bytestring(
        chunk_data_const(entry->key), entry->key->data->size);
    cbor_array_push(entry_item, key_bstr);
    cbor_decref(&key_bstr);

    // has_value flag
    cbor_item_t* has_value = entry->has_value ? cbor_build_bool(true) : cbor_build_bool(false);
    cbor_array_push(entry_item, has_value);
    cbor_decref(&has_value);

    // Value or child
    if (entry->has_value) {
      if (entry->has_versions) {
        // MVCC: Serialize version chain
        // Count versions
        size_t version_count = 0;
        version_entry_t* current = entry->versions;
        while (current != NULL) {
          version_count++;
          current = current->next;
        }

        // Create array of versions: [[txn_id, is_deleted, value], ...]
        cbor_item_t* versions_array = cbor_new_definite_array(version_count);
        if (versions_array == NULL) {
          cbor_decref(&entries);
          cbor_decref(&entry_item);
          return NULL;
        }

        current = entry->versions;
        while (current != NULL) {
          cbor_item_t* version_item = cbor_new_definite_array(3);
          if (version_item == NULL) {
            cbor_decref(&versions_array);
            cbor_decref(&entries);
            cbor_decref(&entry_item);
            return NULL;
          }

          // Transaction ID: [time, nanos, count]
          cbor_item_t* txn_id = cbor_new_definite_array(3);
          cbor_item_t* time = cbor_build_uint64(current->txn_id.time);
          cbor_item_t* nanos = cbor_build_uint64(current->txn_id.nanos);
          cbor_item_t* counter = cbor_build_uint64(current->txn_id.count);
          cbor_array_push(txn_id, time);
          cbor_decref(&time);
          cbor_array_push(txn_id, nanos);
          cbor_decref(&nanos);
          cbor_array_push(txn_id, counter);
          cbor_decref(&counter);
          cbor_array_push(version_item, txn_id);
          cbor_decref(&txn_id);

          // is_deleted flag
          cbor_item_t* is_deleted = cbor_build_bool(current->is_deleted);
          cbor_array_push(version_item, is_deleted);
          cbor_decref(&is_deleted);

          // value (can be null for tombstones)
          if (current->value != NULL) {
            cbor_item_t* value_cbor = identifier_to_cbor(current->value);
            cbor_array_push(version_item, value_cbor);
            cbor_decref(&value_cbor);
          } else {
            cbor_item_t* null_val = cbor_new_null();
            cbor_array_push(version_item, null_val);
            cbor_decref(&null_val);
          }

          cbor_array_push(versions_array, version_item);
          cbor_decref(&version_item);
          current = current->next;
        }

        cbor_array_push(entry_item, versions_array);
        cbor_decref(&versions_array);
      } else {
        // Legacy: Single value
        cbor_item_t* value_cbor = identifier_to_cbor(entry->value);
        cbor_array_push(entry_item, value_cbor);
        cbor_decref(&value_cbor);
      }
    } else {
      cbor_item_t* child_cbor = hbtrie_node_to_cbor(entry->child);
      cbor_array_push(entry_item, child_cbor);
      cbor_decref(&child_cbor);
    }

    cbor_array_push(entries, entry_item);
    cbor_decref(&entry_item);
  }

  return entries;
}

// Helper function to find a value by key in a CBOR map
static cbor_item_t* map_get_value(cbor_item_t* map, const char* key) {
  if (!cbor_isa_map(map)) return NULL;

  struct cbor_pair* pairs = cbor_map_handle(map);
  size_t map_size = cbor_map_size(map);

  for (size_t i = 0; i < map_size; i++) {
    if (cbor_isa_string(pairs[i].key)) {
      size_t key_len = cbor_string_length(pairs[i].key);
      const char* key_str = (const char*)cbor_string_handle(pairs[i].key);
      if (key_len == strlen(key) && memcmp(key_str, key, key_len) == 0) {
        return pairs[i].value;
      }
    }
  }
  return NULL;
}

static hbtrie_node_t* cbor_to_hbtrie_node(cbor_item_t* item, uint32_t btree_node_size) {
  if (item == NULL || cbor_is_null(item)) {
    return NULL;
  }

  if (!cbor_isa_array(item)) {
    return NULL;
  }

  hbtrie_node_t* node = hbtrie_node_create(btree_node_size);
  if (node == NULL) return NULL;

  size_t num_entries = cbor_array_size(item);
  for (size_t i = 0; i < num_entries; i++) {
    cbor_item_t* entry_item = cbor_array_get(item, i);
    if (!cbor_isa_array(entry_item) || cbor_array_size(entry_item) != 3) {
      cbor_decref(&entry_item);
      hbtrie_node_destroy(node);
      return NULL;
    }

    bnode_entry_t entry = {0};

    // Key
    cbor_item_t* key_item = cbor_array_get(entry_item, 0);
    if (!cbor_isa_bytestring(key_item)) {
      cbor_decref(&key_item);
      cbor_decref(&entry_item);
      hbtrie_node_destroy(node);
      return NULL;
    }
    entry.key = chunk_create(cbor_bytestring_handle(key_item), cbor_bytestring_length(key_item));
    cbor_decref(&key_item);
    if (entry.key == NULL) {
      cbor_decref(&entry_item);
      hbtrie_node_destroy(node);
      return NULL;
    }

    // has_value
    cbor_item_t* has_value_item = cbor_array_get(entry_item, 1);
    entry.has_value = cbor_is_bool(has_value_item) && cbor_get_bool(has_value_item);
    cbor_decref(&has_value_item);

    // Value or child
    cbor_item_t* value_or_child = cbor_array_get(entry_item, 2);
    if (entry.has_value) {
      // Check if it's a version chain (array of versions) or single value (bytestring)
      if (cbor_isa_array(value_or_child)) {
        // MVCC: Deserialize version chain
        entry.has_versions = 1;
        entry.versions = NULL;

        size_t num_versions = cbor_array_size(value_or_child);
        version_entry_t* prev_version = NULL;

        for (size_t j = 0; j < num_versions; j++) {
          cbor_item_t* version_item = cbor_array_get(value_or_child, j);
          if (!cbor_isa_array(version_item) || cbor_array_size(version_item) != 3) {
            cbor_decref(&version_item);
            cbor_decref(&value_or_child);
            cbor_decref(&entry_item);
            hbtrie_node_destroy(node);
            return NULL;
          }

          // Transaction ID: [time, nanos, count]
          cbor_item_t* txn_id_item = cbor_array_get(version_item, 0);
          if (!cbor_isa_array(txn_id_item) || cbor_array_size(txn_id_item) != 3) {
            cbor_decref(&txn_id_item);
            cbor_decref(&version_item);
            cbor_decref(&value_or_child);
            cbor_decref(&entry_item);
            hbtrie_node_destroy(node);
            return NULL;
          }

          cbor_item_t* time_item = cbor_array_get(txn_id_item, 0);
          cbor_item_t* nanos_item = cbor_array_get(txn_id_item, 1);
          cbor_item_t* counter_item = cbor_array_get(txn_id_item, 2);

          if (!cbor_isa_uint(time_item) || !cbor_isa_uint(nanos_item) || !cbor_isa_uint(counter_item)) {
            cbor_decref(&time_item);
            cbor_decref(&nanos_item);
            cbor_decref(&counter_item);
            cbor_decref(&txn_id_item);
            cbor_decref(&version_item);
            cbor_decref(&value_or_child);
            cbor_decref(&entry_item);
            hbtrie_node_destroy(node);
            return NULL;
          }

          transaction_id_t txn_id;
          txn_id.time = cbor_get_uint64(time_item);
          txn_id.nanos = cbor_get_uint64(nanos_item);
          txn_id.count = cbor_get_uint64(counter_item);
          cbor_decref(&time_item);
          cbor_decref(&nanos_item);
          cbor_decref(&counter_item);
          cbor_decref(&txn_id_item);

          // is_deleted flag
          cbor_item_t* is_deleted_item = cbor_array_get(version_item, 1);
          uint8_t is_deleted = cbor_is_bool(is_deleted_item) && cbor_get_bool(is_deleted_item);
          cbor_decref(&is_deleted_item);

          // value (can be null for tombstones)
          cbor_item_t* value_item = cbor_array_get(version_item, 2);
          identifier_t* value = NULL;
          if (!cbor_is_null(value_item)) {
            value = cbor_to_identifier(value_item, DEFAULT_CHUNK_SIZE);
          }
          cbor_decref(&value_item);

          // Create version entry
          version_entry_t* version = version_entry_create(txn_id, value, is_deleted);
          if (version == NULL) {
            if (value != NULL) identifier_destroy(value);
            cbor_decref(&version_item);
            cbor_decref(&value_or_child);
            cbor_decref(&entry_item);
            hbtrie_node_destroy(node);
            return NULL;
          }

          // Link versions (newest first)
          if (entry.versions == NULL) {
            entry.versions = version;
          } else {
            prev_version->next = version;
            version->prev = prev_version;
          }
          prev_version = version;

          cbor_decref(&version_item);
        }
      } else {
        // Legacy: Single value
        entry.has_versions = 0;
        entry.value = cbor_to_identifier(value_or_child, DEFAULT_CHUNK_SIZE);
      }
    } else {
      entry.child = cbor_to_hbtrie_node(value_or_child, btree_node_size);
    }
    cbor_decref(&value_or_child);

    bnode_insert(node->btree, &entry);
    cbor_decref(&entry_item);
  }

  return node;
}

cbor_item_t* hbtrie_to_cbor(hbtrie_t* trie) {
  if (trie == NULL) return NULL;

  cbor_item_t* root = cbor_new_definite_map(3);
  if (root == NULL) return NULL;

  // chunk_size
  cbor_item_t* chunk_size = cbor_build_uint8(trie->chunk_size);
  cbor_map_add(root, (struct cbor_pair){
      .key = cbor_build_string("chunk_size"),
      .value = chunk_size
  });
  cbor_decref(&chunk_size);

  // btree_node_size
  cbor_item_t* btree_size = cbor_build_uint32(trie->btree_node_size);
  cbor_map_add(root, (struct cbor_pair){
      .key = cbor_build_string("btree_node_size"),
      .value = btree_size
  });
  cbor_decref(&btree_size);

  // root node
  cbor_item_t* root_node = hbtrie_node_to_cbor(trie->root);
  cbor_map_add(root, (struct cbor_pair){
      .key = cbor_build_string("root"),
      .value = root_node
  });
  cbor_decref(&root_node);

  return root;
}

// Helper function to find a string key in a CBOR map
static cbor_item_t* cbor_map_find_key(cbor_item_t* map, const char* key) {
  if (!cbor_isa_map(map)) return NULL;

  size_t map_size = cbor_map_size(map);
  struct cbor_pair* pairs = cbor_map_handle(map);

  for (size_t i = 0; i < map_size; i++) {
    cbor_item_t* map_key = pairs[i].key;
    if (cbor_isa_string(map_key)) {
      size_t key_len = cbor_string_length(map_key);
      const char* key_data = (const char*)cbor_string_handle(map_key);
      if (key_len == strlen(key) && memcmp(key_data, key, key_len) == 0) {
        return pairs[i].value;
      }
    }
  }
  return NULL;
}

hbtrie_t* cbor_to_hbtrie(cbor_item_t* item) {
  if (item == NULL || !cbor_isa_map(item)) {
    return NULL;
  }

  // Get chunk_size
  cbor_item_t* chunk_size_item = cbor_map_find_key(item, "chunk_size");
  if (chunk_size_item == NULL || !cbor_isa_uint(chunk_size_item)) {
    return NULL;
  }
  uint8_t chunk_size = (uint8_t)cbor_get_uint32(chunk_size_item);

  // Get btree_node_size
  cbor_item_t* btree_size_item = cbor_map_find_key(item, "btree_node_size");
  if (btree_size_item == NULL || !cbor_isa_uint(btree_size_item)) {
    return NULL;
  }
  uint32_t btree_node_size = cbor_get_uint32(btree_size_item);

  hbtrie_t* trie = hbtrie_create(chunk_size, btree_node_size);
  if (trie == NULL) return NULL;

  // Get root node
  cbor_item_t* root_item = cbor_map_find_key(item, "root");
  if (root_item != NULL && !cbor_is_null(root_item)) {
    hbtrie_node_t* root_node = cbor_to_hbtrie_node(root_item, btree_node_size);
    if (root_node == NULL) {
      hbtrie_destroy(trie);
      return NULL;
    }
    hbtrie_node_destroy(trie->root);
    trie->root = root_node;
  }

  return trie;
}

uint32_t hbtrie_compute_hash(hbtrie_t* trie) {
  if (trie == NULL) return 0;

  // Serialize to CBOR, then compute hash
  cbor_item_t* cbor = hbtrie_to_cbor(trie);
  if (cbor == NULL) return 0;

  unsigned char* buffer = NULL;
  size_t buffer_size = 0;
  cbor_serialize_alloc(cbor, &buffer, &buffer_size);
  cbor_decref(&cbor);

  if (buffer == NULL) return 0;

  // Simple CRC32 hash
  uint32_t hash = 0xFFFFFFFF;
  for (size_t i = 0; i < buffer_size; i++) {
    hash ^= buffer[i];
    for (int j = 0; j < 8; j++) {
      if (hash & 1) {
        hash = (hash >> 1) ^ 0xEDB88320;
      } else {
        hash >>= 1;
      }
    }
  }
  hash ^= 0xFFFFFFFF;

  free(buffer);
  return hash;
}

int hbtrie_serialize(hbtrie_t* trie, uint8_t** buf, size_t* len) {
  if (trie == NULL || buf == NULL || len == NULL) {
    return -1;
  }

  cbor_item_t* cbor = hbtrie_to_cbor(trie);
  if (cbor == NULL) {
    return -1;
  }

  unsigned char* buffer = NULL;
  size_t buffer_size = 0;
  cbor_serialize_alloc(cbor, &buffer, &buffer_size);
  cbor_decref(&cbor);

  if (buffer == NULL) {
    return -1;
  }

  *buf = buffer;
  *len = buffer_size;
  return 0;
}

hbtrie_t* hbtrie_deserialize(uint8_t* buf, size_t len, uint8_t chunk_size, uint32_t btree_node_size) {
  if (buf == NULL || len == 0) {
    return NULL;
  }

  struct cbor_load_result result;
  cbor_item_t* cbor = cbor_load(buf, len, &result);
  if (cbor == NULL || result.error.code != CBOR_ERR_NONE) {
    if (cbor) cbor_decref(&cbor);
    return NULL;
  }

  hbtrie_t* trie = cbor_to_hbtrie(cbor);
  cbor_decref(&cbor);

  return trie;
}

// ============================================================================
// MVCC Operations
// ============================================================================

identifier_t* hbtrie_find_mvcc(hbtrie_t* trie, path_t* path, transaction_id_t read_txn_id) {
  if (trie == NULL || path == NULL) {
    return NULL;
  }

  // No lock needed for MVCC reads - lock-free!
  hbtrie_node_t* current = trie->root;
  size_t path_len_ids = path_length(path);

  if (path_len_ids == 0) {
    return NULL;
  }

  // Traverse through each identifier in the path
  for (size_t i = 0; i < path_len_ids; i++) {
    identifier_t* identifier = path_get(path, i);
    if (identifier == NULL) {
      return NULL;
    }

    size_t nchunk = identifier_chunk_count(identifier);

    // Traverse through chunks of this identifier
    for (size_t j = 0; j < nchunk; j++) {
      chunk_t* chunk = identifier_get_chunk(identifier, j);
      if (chunk == NULL) {
        return NULL;
      }

      size_t index;
      bnode_entry_t* entry = bnode_find(current->btree, chunk, &index);

      int is_last_chunk = (j == nchunk - 1);
      int is_last_identifier = (i == path_len_ids - 1);

      if (is_last_chunk && is_last_identifier) {
        // Final position - check version chain for visible version
        if (entry == NULL || !entry->has_value) {
          return NULL;  // No entry or no value
        }

        if (entry->has_versions) {
          // MVCC: Find visible version
          version_entry_t* visible = version_entry_find_visible(entry->versions, read_txn_id);
          if (visible == NULL || visible->value == NULL) {
            return NULL;  // Deleted or not visible
          }
          return (identifier_t*)refcounter_reference((refcounter_t*)visible->value);
        } else {
          // Legacy: single value
          if (entry->value == NULL) {
            return NULL;
          }
          return (identifier_t*)refcounter_reference((refcounter_t*)entry->value);
        }
      } else if (is_last_chunk) {
        // End of this identifier, move to next HBTrie level
        if (entry == NULL || entry->has_value || entry->child == NULL) {
          return NULL;
        }
        current = entry->child;
      } else {
        // Intermediate chunk within this identifier
        if (entry == NULL || entry->has_value || entry->child == NULL) {
          return NULL;
        }
        current = entry->child;
      }
    }
  }

  return NULL;
}

identifier_t* hbtrie_find_with_txn(hbtrie_t* trie, path_t* path, txn_desc_t* txn) {
  if (txn == NULL) return NULL;
  return hbtrie_find_mvcc(trie, path, txn->txn_id);
}

int hbtrie_insert_mvcc(hbtrie_t* trie, path_t* path, identifier_t* value, transaction_id_t txn_id) {
  if (trie == NULL || path == NULL || value == NULL) {
    return -1;
  }

  // Acquire write lock (serializes writers)
  platform_lock(&trie->lock);

  hbtrie_node_t* current = trie->root;
  size_t path_len_ids = path_length(path);

  if (path_len_ids == 0) {
    platform_unlock(&trie->lock);
    return -1;
  }

  // Track path for node creation
  vec_t(struct { hbtrie_node_t* node; size_t chunk_index; }) path_stack;
  vec_init(&path_stack);

  // Traverse/create path
  for (size_t i = 0; i < path_len_ids; i++) {
    identifier_t* identifier = path_get(path, i);
    if (identifier == NULL) {
      vec_deinit(&path_stack);
      platform_unlock(&trie->lock);
      return -1;
    }

    size_t nchunk = identifier_chunk_count(identifier);

    for (size_t j = 0; j < nchunk; j++) {
      chunk_t* chunk = identifier_get_chunk(identifier, j);
      if (chunk == NULL) {
        vec_deinit(&path_stack);
        platform_unlock(&trie->lock);
        return -1;
      }

      size_t index;
      bnode_entry_t* entry = bnode_find(current->btree, chunk, &index);

      int is_last_chunk = (j == nchunk - 1);
      int is_last_identifier = (i == path_len_ids - 1);

      if (is_last_chunk && is_last_identifier) {
        // Final position - insert value with version chain
        if (entry == NULL) {
          // Create new entry
          log_info("MVCC: Creating NEW entry for path (txn=%lu.%09lu.%lu)",
                  txn_id.time, txn_id.nanos, txn_id.count);
          bnode_entry_t new_entry = {0};
          new_entry.key = chunk_share(chunk);
          new_entry.has_value = 1;
          new_entry.has_versions = 0;  // Legacy mode for first value
          new_entry.value = (identifier_t*)refcounter_reference((refcounter_t*)value);
          new_entry.value_txn_id = txn_id;  // Store transaction ID

          if (bnode_insert(current->btree, &new_entry) != 0) {
            chunk_destroy(new_entry.key);
            vec_deinit(&path_stack);
            platform_unlock(&trie->lock);
            return -1;
          }
        } else {
          // Entry exists - upgrade to version chain or add version
          log_info("MVCC: Found EXISTING entry for path (has_value=%d, has_versions=%d, txn=%lu.%09lu.%lu)",
                  entry->has_value, entry->has_versions, txn_id.time, txn_id.nanos, txn_id.count);
          // Entry exists - upgrade to version chain or add version
          if (!entry->has_value) {
            // Entry exists but no value - set first value (legacy mode)
            entry->has_value = 1;
            entry->has_versions = 0;
            entry->value = (identifier_t*)refcounter_reference((refcounter_t*)value);
            entry->value_txn_id = txn_id;  // Store transaction ID
          } else if (entry->has_versions) {
            // Already has version chain - add new version
            identifier_t* new_value_ref = (identifier_t*)refcounter_reference((refcounter_t*)value);
            if (version_entry_add(&entry->versions, txn_id, new_value_ref, 0) != 0) {
              identifier_destroy(new_value_ref);
              vec_deinit(&path_stack);
              platform_unlock(&trie->lock);
              return -1;
            }
          } else {
            // Legacy single value - upgrade to version chain
            log_info("MVCC: Upgrading legacy entry to version chain (old_txn=%lu.%09lu.%lu, new_txn=%lu.%09lu.%lu)",
                    entry->value_txn_id.time, entry->value_txn_id.nanos, entry->value_txn_id.count,
                    txn_id.time, txn_id.nanos, txn_id.count);
            // Save the value pointer before upgrading (union will be reused)
            identifier_t* old_value = entry->value;

            version_entry_t* old_version = version_entry_create(
                entry->value_txn_id,  // Use stored txn_id
                old_value,
                0
            );
            if (old_version == NULL) {
              vec_deinit(&path_stack);
              platform_unlock(&trie->lock);
              return -1;
            }

            entry->versions = old_version;
            entry->has_versions = 1;
            // entry->value is now in the union with versions, so we don't need to set it

            // Add new version
            identifier_t* new_value_ref = (identifier_t*)refcounter_reference((refcounter_t*)value);
            log_info("MVCC: Added new version to chain (txn=%lu.%09lu.%lu)",
                    txn_id.time, txn_id.nanos, txn_id.count);
            if (version_entry_add(&entry->versions, txn_id, new_value_ref, 0) != 0) {
              identifier_destroy(new_value_ref);
              version_entry_destroy(old_version);
              entry->has_versions = 0;
              entry->versions = NULL;
              vec_deinit(&path_stack);
              platform_unlock(&trie->lock);
              return -1;
            }
          }
        }
      } else if (is_last_chunk) {
        // End of identifier - move to next HBTrie level
        if (entry == NULL) {
          // Create child node
          hbtrie_node_t* child = hbtrie_node_create(trie->btree_node_size);
          if (child == NULL) {
            vec_deinit(&path_stack);
            platform_unlock(&trie->lock);
            return -1;
          }

          bnode_entry_t new_entry = {0};
          new_entry.key = chunk_share(chunk);
          new_entry.has_value = 0;
          new_entry.child = child;

          if (bnode_insert(current->btree, &new_entry) != 0) {
            chunk_destroy(new_entry.key);
            hbtrie_node_destroy(child);
            vec_deinit(&path_stack);
            platform_unlock(&trie->lock);
            return -1;
          }

          current = child;
        } else if (entry->has_value) {
          // Entry exists but has value instead of child
          vec_deinit(&path_stack);
          platform_unlock(&trie->lock);
          return -1;
        } else {
          current = entry->child;
        }
      } else {
        // Intermediate chunk - move deeper
        if (entry == NULL) {
          // Create child node
          hbtrie_node_t* child = hbtrie_node_create(trie->btree_node_size);
          if (child == NULL) {
            vec_deinit(&path_stack);
            platform_unlock(&trie->lock);
            return -1;
          }

          bnode_entry_t new_entry = {0};
          new_entry.key = chunk_share(chunk);
          new_entry.has_value = 0;
          new_entry.child = child;

          if (bnode_insert(current->btree, &new_entry) != 0) {
            chunk_destroy(new_entry.key);
            hbtrie_node_destroy(child);
            vec_deinit(&path_stack);
            platform_unlock(&trie->lock);
            return -1;
          }

          current = child;
        } else if (entry->has_value) {
          vec_deinit(&path_stack);
          platform_unlock(&trie->lock);
          return -1;
        } else {
          current = entry->child;
        }
      }
    }
  }

  vec_deinit(&path_stack);
  platform_unlock(&trie->lock);
  return 0;
}

identifier_t* hbtrie_delete_mvcc(hbtrie_t* trie, path_t* path, transaction_id_t txn_id) {
  if (trie == NULL || path == NULL) {
    return NULL;
  }

  platform_lock(&trie->lock);

  // Traverse to find the entry
  hbtrie_node_t* current = trie->root;
  size_t path_len_ids = path_length(path);

  if (path_len_ids == 0) {
    platform_unlock(&trie->lock);
    return NULL;
  }

  // Navigate to the final position
  for (size_t i = 0; i < path_len_ids; i++) {
    identifier_t* identifier = path_get(path, i);
    if (identifier == NULL) {
      platform_unlock(&trie->lock);
      return NULL;
    }

    size_t nchunk = identifier_chunk_count(identifier);

    for (size_t j = 0; j < nchunk; j++) {
      chunk_t* chunk = identifier_get_chunk(identifier, j);
      if (chunk == NULL) {
        platform_unlock(&trie->lock);
        return NULL;
      }

      size_t index;
      bnode_entry_t* entry = bnode_find(current->btree, chunk, &index);

      int is_last_chunk = (j == nchunk - 1);
      int is_last_identifier = (i == path_len_ids - 1);

      if (is_last_chunk && is_last_identifier) {
        // Final position - create tombstone version
        if (entry == NULL || !entry->has_value) {
          // No entry or no value to delete
          platform_unlock(&trie->lock);
          return NULL;
        }

        identifier_t* last_visible = NULL;

        if (entry->has_versions) {
          // Has version chain - find last visible version to return
          version_entry_t* visible = version_entry_find_visible(entry->versions, txn_id);
          if (visible != NULL && !visible->is_deleted) {
            last_visible = (identifier_t*)refcounter_reference((refcounter_t*)visible->value);
          }

          // Add tombstone version
          if (version_entry_add(&entry->versions, txn_id, NULL, 1) != 0) {
            if (last_visible) identifier_destroy(last_visible);
            platform_unlock(&trie->lock);
            return NULL;
          }
        } else {
          // Legacy single value - upgrade to version chain with tombstone
          if (entry->value != NULL) {
            last_visible = (identifier_t*)refcounter_reference((refcounter_t*)entry->value);

            // Create version chain with old value and tombstone
            version_entry_t* old_version = version_entry_create(
                entry->value_txn_id,
                entry->value,
                0
            );
            if (old_version == NULL) {
              if (last_visible) identifier_destroy(last_visible);
              platform_unlock(&trie->lock);
              return NULL;
            }

            entry->versions = old_version;
            entry->has_versions = 1;
            // entry->value and entry->versions share memory via union, so don't set value to NULL

            // Add tombstone version
            if (version_entry_add(&entry->versions, txn_id, NULL, 1) != 0) {
              version_entry_destroy(old_version);
              entry->has_versions = 0;
              entry->versions = NULL;
              if (last_visible) identifier_destroy(last_visible);
              platform_unlock(&trie->lock);
              return NULL;
            }
          }
        }

        platform_unlock(&trie->lock);
        return last_visible;
      } else if (is_last_chunk) {
        // End of identifier - move to next HBTrie level
        if (entry == NULL || entry->has_value || entry->child == NULL) {
          platform_unlock(&trie->lock);
          return NULL;
        }
        current = entry->child;
      } else {
        // Intermediate chunk
        if (entry == NULL || entry->has_value || entry->child == NULL) {
          platform_unlock(&trie->lock);
          return NULL;
        }
        current = entry->child;
      }
    }
  }

  platform_unlock(&trie->lock);
  return NULL;
}

size_t hbtrie_gc(hbtrie_t* trie, transaction_id_t min_active_txn_id) {
  if (trie == NULL || trie->root == NULL) {
    return 0;
  }

  // Traverse all nodes and clean up version chains
  size_t total_removed = 0;

  // Use a stack for iterative traversal
  vec_t(hbtrie_node_t*) stack;
  vec_init(&stack);
  vec_push(&stack, trie->root);

  while (stack.length > 0) {
    hbtrie_node_t* node = vec_pop(&stack);
    if (node == NULL) continue;

    // Process all entries in this node
    for (size_t i = 0; i < node->btree->entries.length; i++) {
      bnode_entry_t* entry = &node->btree->entries.data[i];

      if (entry->has_value && entry->has_versions) {
        // Clean up old versions
        size_t removed = version_entry_gc(&entry->versions, min_active_txn_id);
        total_removed += removed;

        // If only one version remains and it's not deleted, downgrade to legacy mode
        if (entry->versions != NULL &&
            entry->versions->next == NULL &&
            !entry->versions->is_deleted) {
          // Single non-deleted version - convert to legacy
          entry->value = (identifier_t*)refcounter_reference((refcounter_t*)entry->versions->value);
          entry->value_txn_id = entry->versions->txn_id;
          version_entry_destroy(entry->versions);
          entry->has_versions = 0;
        }
      }

      // Add child nodes to stack for traversal
      if (!entry->has_value && entry->child != NULL) {
        vec_push(&stack, entry->child);
      }
    }
  }

  vec_deinit(&stack);
  return total_removed;
}
