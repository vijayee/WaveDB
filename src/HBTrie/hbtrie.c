//
// Created by victor on 3/11/26.
//

#include "hbtrie.h"
#include "../Util/allocator.h"
#include <string.h>

#define DEFAULT_BTREE_NODE_SIZE 4096

hbtrie_t* hbtrie_create(uint8_t chunk_size, uint32_t btree_node_size) {
  if (chunk_size == 0) {
    chunk_size = DEFAULT_CHUNK_SIZE;
  }
  if (btree_node_size == 0) {
    btree_node_size = DEFAULT_BTREE_NODE_SIZE;
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
 * Traverse through all chunks of a single identifier within one HBTrie node.
 * Returns the entry at the last chunk, or NULL if not found.
 * Creates missing nodes if create is true.
 */
static bnode_entry_t* traverse_identifier_chunks(
    hbtrie_t* trie,
    hbtrie_node_t** node,
    identifier_t* identifier,
    int create,
    hbtrie_node_t** last_child_out
) {
  hbtrie_node_t* current = *node;
  size_t nchunk = identifier_chunk_count(identifier);

  for (size_t i = 0; i < nchunk; i++) {
    chunk_t* chunk = identifier_get_chunk(identifier, i);
    if (chunk == NULL) {
      return NULL;
    }

    size_t index;
    bnode_entry_t* entry = bnode_find(current->btree, chunk, &index);

    if (i == nchunk - 1) {
      // Last chunk of this identifier - return the entry
      if (entry == NULL && create) {
        // Create new entry - will be filled in by caller
        bnode_entry_t new_entry = {0};
        new_entry.key = chunk_create(chunk_data_const(chunk), trie->chunk_size);
        bnode_insert(current->btree, &new_entry);
        entry = bnode_find(current->btree, chunk, &index);
      }
      if (last_child_out) {
        *last_child_out = current;
      }
      return entry;
    }

    // Not last chunk - need to traverse or create intermediate node
    if (entry == NULL) {
      if (!create) {
        return NULL;
      }
      // Create intermediate HBTrie node for chunk chain
      // This is internal to this identifier, not a new level
      // For now, we store a child pointer
      hbtrie_node_t* child = hbtrie_node_create(trie->btree_node_size);
      if (child == NULL) {
        return NULL;
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
      // Entry exists with a value (leaf) - can't traverse further
      return NULL;
    }
  }

  if (last_child_out) {
    *last_child_out = current;
  }
  return NULL;
}

int hbtrie_insert(hbtrie_t* trie, path_t* path, identifier_t* value) {
  if (trie == NULL || path == NULL || value == NULL) {
    return -1;
  }

  platform_lock(&trie->lock);

  hbtrie_node_t* current = trie->root;
  size_t path_len = path_length(path);

  if (path_len == 0) {
    // Empty path - can't insert
    platform_unlock(&trie->lock);
    return -1;
  }

  // Traverse through each identifier in the path
  for (size_t i = 0; i < path_len; i++) {
    identifier_t* identifier = path_get(path, i);
    if (identifier == NULL) {
      platform_unlock(&trie->lock);
      return -1;
    }

    size_t nchunk = identifier_chunk_count(identifier);

    // Traverse through chunks of this identifier
    for (size_t j = 0; j < nchunk; j++) {
      chunk_t* chunk = identifier_get_chunk(identifier, j);
      if (chunk == NULL) {
        platform_unlock(&trie->lock);
        return -1;
      }

      size_t index;
      bnode_entry_t* entry = bnode_find(current->btree, chunk, &index);

      int is_last_chunk = (j == nchunk - 1);
      int is_last_identifier = (i == path_len - 1);

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
          // Traverse to existing child
          current = entry->child;
        } else {
          // Entry has a value - need to create subtree
          // For now, we don't support this case
          platform_unlock(&trie->lock);
          return -1;
        }
      } else {
        // Intermediate chunk within this identifier
        if (entry == NULL) {
          // Create intermediate node (same HBTrie level, continuing chunk chain)
          hbtrie_node_t* child = hbtrie_node_create(trie->btree_node_size);
          if (child == NULL) {
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
          platform_unlock(&trie->lock);
          return -1;
        }
      }
    }
  }

  platform_unlock(&trie->lock);
  return 0;
}

identifier_t* hbtrie_find(hbtrie_t* trie, path_t* path) {
  if (trie == NULL || path == NULL) {
    return NULL;
  }

  platform_lock(&trie->lock);

  hbtrie_node_t* current = trie->root;
  size_t path_len = path_length(path);

  if (path_len == 0) {
    platform_unlock(&trie->lock);
    return NULL;
  }

  // Traverse through each identifier in the path
  for (size_t i = 0; i < path_len; i++) {
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

      if (entry == NULL) {
        platform_unlock(&trie->lock);
        return NULL;
      }

      int is_last_chunk = (j == nchunk - 1);
      int is_last_identifier = (i == path_len - 1);

      if (is_last_chunk && is_last_identifier) {
        // Final position - return the value
        if (entry->has_value && entry->value != NULL) {
          identifier_t* result = (identifier_t*)refcounter_reference((refcounter_t*)entry->value);
          platform_unlock(&trie->lock);
          return result;
        }
        platform_unlock(&trie->lock);
        return NULL;
      }

      // Need to continue traversing
      if (entry->has_value || entry->child == NULL) {
        // Can't traverse further
        platform_unlock(&trie->lock);
        return NULL;
      }

      current = entry->child;
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

  size_t path_len = path_length(path);
  if (path_len == 0) {
    platform_unlock(&trie->lock);
    return NULL;
  }

  // Track the path for cleanup
  // Each entry records: the parent node, the entry in that node's btree,
  // and the chunk key used to find it
  #define MAX_PATH_DEPTH 256
  struct {
    hbtrie_node_t* parent_node;  // The node containing this entry
    bnode_entry_t* entry;         // The entry we traversed
    chunk_t* chunk;               // The chunk key for this entry
  } traversal[MAX_PATH_DEPTH];
  int traversal_len = 0;

  hbtrie_node_t* current = trie->root;

  // Traverse the path
  for (size_t i = 0; i < path_len; i++) {
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

      if (entry == NULL) {
        platform_unlock(&trie->lock);
        return NULL;
      }

      int is_last_chunk = (j == nchunk - 1);
      int is_last_identifier = (i == path_len - 1);

      // Record traversal for cleanup (before moving to child)
      if (traversal_len < MAX_PATH_DEPTH) {
        traversal[traversal_len].parent_node = current;
        traversal[traversal_len].entry = entry;
        traversal[traversal_len].chunk = chunk;
        traversal_len++;
      }

      if (is_last_chunk && is_last_identifier) {
        // Found the target - remove the value
        if (!entry->has_value || entry->value == NULL) {
          platform_unlock(&trie->lock);
          return NULL;
        }

        identifier_t* result = entry->value;
        entry->has_value = 0;
        entry->value = NULL;

        // Pop the last traversal entry since it's the leaf (no child to clean up)
        traversal_len--;

        // Remove the entry from the leaf btree if it has no child
        if (entry->child == NULL) {
          // Entry was a leaf with just a value - remove it from btree
          hbtrie_node_t* parent = traversal[traversal_len].parent_node;
          chunk_t* leaf_chunk = traversal[traversal_len].chunk;
          bnode_remove(parent->btree, leaf_chunk);
        }

        // Now clean up empty nodes going back up
        for (int k = traversal_len - 1; k >= 0; k--) {
          bnode_entry_t* ent = traversal[k].entry;

          // Check if the entry has a child and if that child is empty
          if (!ent->has_value && ent->child != NULL) {
            hbtrie_node_t* child = ent->child;

            // If child node's btree is empty, remove it
            if (bnode_is_empty(child->btree)) {
              hbtrie_node_destroy(child);
              ent->child = NULL;
              ent->has_value = 0;

              // Remove this entry from the parent's btree
              hbtrie_node_t* parent = traversal[k].parent_node;
              bnode_remove(parent->btree, traversal[k].chunk);
            } else {
              // Child has entries, stop cleanup
              break;
            }
          } else if (ent->has_value) {
            // Entry has a value (another leaf), stop cleanup
            break;
          }
          // If entry has no value and no child, continue checking parent
        }

        platform_unlock(&trie->lock);
        return result;
      }

      if (entry->has_value || entry->child == NULL) {
        platform_unlock(&trie->lock);
        return NULL;
      }

      current = entry->child;
    }
  }

  platform_unlock(&trie->lock);
  return NULL;
}

void hbtrie_cursor_init(hbtrie_cursor_t* cursor, hbtrie_t* trie, path_t* path) {
  if (cursor == NULL || trie == NULL) return;

  cursor->current = trie->root;
  cursor->identifier_index = 0;
  cursor->chunk_pos = 0;
}

int hbtrie_cursor_next(hbtrie_cursor_t* cursor) {
  if (cursor == NULL || cursor->current == NULL) return -1;

  cursor->chunk_pos++;
  return 0;
}

int hbtrie_cursor_at_end(hbtrie_cursor_t* cursor) {
  if (cursor == NULL) return 1;
  return 0;
}

chunk_t* hbtrie_cursor_get_chunk(hbtrie_cursor_t* cursor) {
  if (cursor == NULL || cursor->current == NULL) return NULL;
  // TODO: Implement proper chunk retrieval based on path
  return NULL;
}

hbtrie_node_t* hbtrie_cursor_get_node(hbtrie_cursor_t* cursor) {
  if (cursor == NULL) return NULL;
  return cursor->current;
}

hbtrie_node_t* hbtrie_node_copy(hbtrie_node_t* node) {
  if (node == NULL) return NULL;

  // Iterative copy using a dynamic work stack
  typedef struct {
    hbtrie_node_t* src;
    hbtrie_node_t* dst;
  } copy_work_t;

  vec_t(copy_work_t) stack;
  vec_init(&stack);

  // Create root copy
  hbtrie_node_t* root_copy = hbtrie_node_create(node->btree->node_size);
  if (root_copy == NULL) {
    vec_deinit(&stack);
    return NULL;
  }

  // Push initial work
  copy_work_t initial = {node, root_copy};
  if (vec_push(&stack, initial) != 0) {
    hbtrie_node_destroy(root_copy);
    vec_deinit(&stack);
    return NULL;
  }

  // Process all nodes iteratively
  while (stack.length > 0) {
    copy_work_t work = stack.data[--stack.length];
    hbtrie_node_t* src = work.src;
    hbtrie_node_t* dst = work.dst;

    // Copy all entries from source btree to destination btree
    for (int i = 0; i < src->btree->entries.length; i++) {
      bnode_entry_t* src_entry = &src->btree->entries.data[i];
      bnode_entry_t dst_entry = {0};

      // Share the chunk key by reference (chunks are immutable)
      dst_entry.key = chunk_share(src_entry->key);
      if (dst_entry.key == NULL && src_entry->key != NULL) {
        hbtrie_node_destroy(root_copy);
        vec_deinit(&stack);
        return NULL;
      }

      dst_entry.has_value = src_entry->has_value;

      if (src_entry->has_value) {
        // Share identifier value by reference
        if (src_entry->value != NULL) {
          dst_entry.value = (identifier_t*)refcounter_reference((refcounter_t*)src_entry->value);
        }
      } else {
        // Copy child node
        if (src_entry->child != NULL) {
          hbtrie_node_t* child_copy = hbtrie_node_create(src->btree->node_size);
          if (child_copy == NULL) {
            hbtrie_node_destroy(root_copy);
            vec_deinit(&stack);
            return NULL;
          }
          dst_entry.child = child_copy;

          // Push child work onto stack
          copy_work_t child_work = {src_entry->child, child_copy};
          if (vec_push(&stack, child_work) != 0) {
            hbtrie_node_destroy(child_copy);
            hbtrie_node_destroy(root_copy);
            vec_deinit(&stack);
            return NULL;
          }
        }
      }

      // Insert into destination btree
      if (vec_push(&dst->btree->entries, dst_entry) != 0) {
        chunk_destroy(dst_entry.key);
        hbtrie_node_destroy(root_copy);
        vec_deinit(&stack);
        return NULL;
      }
    }
  }

  vec_deinit(&stack);
  return root_copy;
}

hbtrie_t* hbtrie_copy(hbtrie_t* trie) {
  if (trie == NULL) return NULL;

  hbtrie_t* copy = hbtrie_create(trie->chunk_size, trie->btree_node_size);
  if (copy == NULL) return NULL;

  // Copy the root node iteratively
  if (trie->root != NULL) {
    hbtrie_node_t* root_copy = hbtrie_node_copy(trie->root);
    if (root_copy == NULL) {
      hbtrie_destroy(copy);
      return NULL;
    }

    // Replace the root in the copy
    hbtrie_node_destroy(copy->root);
    copy->root = root_copy;
  }

  return copy;
}