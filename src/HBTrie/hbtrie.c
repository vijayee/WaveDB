//
// Created by victor on 3/11/26.
//

#include "hbtrie.h"
#include "../Util/allocator.h"
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

  refcounter_init((refcounter_t*)node);
  platform_lock_init(&node->lock);

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