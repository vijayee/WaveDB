//
// Created by victor on 3/11/26.
//

#include "hbtrie.h"
#include "mvcc.h"
#include "bnode.h"
#include "../Util/allocator.h"
#include "../Util/memory_pool.h"
#include "../Util/log.h"
#include "../Storage/sections.h"
#include <cbor.h>
#include <string.h>

// Default: enough space for ~64 entries per node
// Each entry is roughly: sizeof(bnode_entry_t) + chunk_size for key buffer
#define DEFAULT_ENTRIES_PER_NODE 64

// Maximum B+tree height (safety limit for descent)
#define MAX_BTREE_HEIGHT 32

// Descent path through internal B+tree nodes within a single hbtrie_node.
// Used for write operations that need to propagate splits upward.
typedef struct {
    bnode_t* nodes[MAX_BTREE_HEIGHT];  // Bnodes from root to leaf
    size_t count;                        // Number of bnodes in path
} btree_path_t;

// Descend through a multi-level B+tree to the leaf node, tracking the path.
// For single-level trees (height == 1), this is a no-op.
static bnode_t* btree_descend_with_path(bnode_t* root, chunk_t* key, btree_path_t* path) {
  if (path) {
    path->count = 0;
  }

  bnode_t* current = root;

  while (atomic_load(&current->level) > 1) {
    if (path && path->count < MAX_BTREE_HEIGHT) {
      path->nodes[path->count++] = current;
    }

    size_t index;
    bnode_entry_t* entry = bnode_find(current, key, &index);

    if (entry != NULL && entry->is_bnode_child && entry->child_bnode != NULL) {
      current = entry->child_bnode;
    } else {
      // No exact match - follow greatest key <= search key
      if (index == 0) {
        current = current->entries.data[0].child_bnode;
      } else {
        current = current->entries.data[index - 1].child_bnode;
      }
    }
  }

  return current;
}

/**
 * Propagate a B+tree split upward through internal nodes.
 *
 * When a leaf bnode overflows and splits, this function inserts the
 * split key and right sibling into the parent bnode. If the parent
 * also overflows, it splits recursively.
 *
 * If the root bnode of the hbtrie_node's B+tree splits, a new root
 * bnode is created and the hbtrie_node's btree_height is incremented.
 *
 * @param hb_node     The hbtrie_node whose B+tree may need split propagation
 * @param bnode_path  Path of internal bnodes from root to leaf (from descent)
 * @param split_key   Key from the initial leaf split (caller retains ownership)
 * @param right_node  Right sibling from the initial leaf split (caller gives ownership)
 * @return 0 on success, -1 on failure
 */
static int btree_propagate_split(hbtrie_node_t* hb_node,
                                  btree_path_t* bnode_path,
                                  chunk_t* split_key,
                                  bnode_t* right_node,
                                  uint8_t chunk_size) {
  if (hb_node == NULL || split_key == NULL || right_node == NULL) {
    return -1;
  }

  chunk_t* current_split_key = split_key;
  bnode_t* current_right = right_node;

  // Walk up the bnode path, propagating splits
  for (int i = (int)bnode_path->count - 1; i >= 0; i--) {
    bnode_t* parent = bnode_path->nodes[i];

    // Insert split key + right child into parent
    int result = bnode_insert_bnode_child(parent, current_split_key, current_right);
    if (result != 0) {
      // Clean up on failure
      if (current_split_key != split_key) chunk_destroy(current_split_key);
      bnode_destroy_tree(current_right);
      return -1;
    }

    // Check if parent needs splitting
    if (!bnode_needs_split(parent, chunk_size)) {
      // No more splits needed
      if (current_split_key != split_key) chunk_destroy(current_split_key);
      return 0;
    }

    // Parent also needs splitting
    chunk_t* parent_split_key = NULL;
    bnode_t* parent_right = NULL;

    if (bnode_split(parent, &parent_right, &parent_split_key) != 0) {
      // Split failed - data is still consistent (parent is just full)
      return 0;
    }

    // Move up: parent's split key and right sibling become the current ones
    if (current_split_key != split_key) chunk_destroy(current_split_key);
    current_split_key = parent_split_key;
    current_right = parent_right;
  }

  // We've reached the root bnode of this hbtrie_node's B+tree.
  // The root has split: create a new root bnode.
  bnode_t* old_root = hb_node->btree;

  bnode_t* new_root = bnode_create_with_level(hb_node->btree->node_size,
                                               atomic_load(&old_root->level) + 1);
  if (new_root == NULL) {
    // Failed to create new root - undo the split
    // The old root and current_right are valid, just oversized
    if (current_split_key != split_key) chunk_destroy(current_split_key);
    bnode_destroy_tree(current_right);
    return -1;
  }

  // Insert left child (old root) and right child into new root
  bnode_entry_t* left_first = bnode_get(old_root, 0);
  if (left_first != NULL) {
    bnode_entry_t left_entry = {0};
    bnode_entry_set_key(&left_entry, bnode_entry_get_key(left_first));
    left_entry.is_bnode_child = 1;
    left_entry.child_bnode = old_root;
    left_entry.has_value = 0;
    bnode_insert(new_root, &left_entry);
  }

  bnode_entry_t right_entry = {0};
  chunk_t* right_key = current_split_key != split_key ? current_split_key : chunk_share(split_key);
  bnode_entry_set_key(&right_entry, right_key);
  right_entry.is_bnode_child = 1;
  right_entry.child_bnode = current_right;
  right_entry.has_value = 0;
  bnode_insert(new_root, &right_entry);

  // Update the hbtrie_node
  hb_node->btree = new_root;
  hb_node->btree_height = atomic_load(&old_root->level) + 1;

  if (current_split_key != split_key) {
    // current_split_key was already consumed by the new root entry
    // (we shared it, so we don't need to destroy it separately)
  }

  return 0;
}

/**
 * Check if a bnode needs splitting after an insert, and propagate the split.
 *
 * If the leaf bnode overflows, splits it and inserts the separator key and
 * right sibling into the parent (from bnode_path). Cascades upward if needed.
 *
 * @param hb_node     The hbtrie_node containing the B+tree
 * @param leaf        The leaf bnode that was just inserted into
 * @param bnode_path  Path of internal bnodes from root to leaf (from descent)
 * @param chunk_size  Chunk size for the trie (used by bnode_needs_split)
 */
static void btree_split_after_insert(hbtrie_node_t* hb_node,
                                      bnode_t* leaf,
                                      btree_path_t* bnode_path,
                                      uint8_t chunk_size) {
  if (!bnode_needs_split(leaf, chunk_size)) {
    return;
  }

  // Split the leaf
  chunk_t* split_key = NULL;
  bnode_t* right_bnode = NULL;

  if (bnode_split(leaf, &right_bnode, &split_key) != 0) {
    return;  // Split failed, data still consistent
  }

  // Propagate the split upward
  if (bnode_path->count == 0) {
    // Leaf IS the root - create a new root
    bnode_t* old_root = hb_node->btree;
    bnode_t* new_root = bnode_create_with_level(old_root->node_size,
                                                  atomic_load(&old_root->level) + 1);
    if (new_root == NULL) {
      chunk_destroy(split_key);
      bnode_destroy_tree(right_bnode);
      return;
    }

    // Add left child (old root)
    bnode_entry_t* left_first = bnode_get(old_root, 0);
    if (left_first != NULL) {
      bnode_entry_t left_entry = {0};
      bnode_entry_set_key(&left_entry, bnode_entry_get_key(left_first));
      left_entry.is_bnode_child = 1;
      left_entry.child_bnode = old_root;
      left_entry.has_value = 0;
      bnode_insert(new_root, &left_entry);
    }

    // Add right child
    bnode_entry_t right_entry = {0};
    bnode_entry_set_key(&right_entry, split_key);
    right_entry.is_bnode_child = 1;
    right_entry.child_bnode = right_bnode;
    right_entry.has_value = 0;
    bnode_insert(new_root, &right_entry);

    hb_node->btree = new_root;
    hb_node->btree_height = atomic_load(&old_root->level) + 1;
  } else {
    // Insert into parent and propagate
    btree_propagate_split(hb_node, bnode_path, split_key, right_bnode, chunk_size);
    // btree_propagate_split shares split_key into parent entries via chunk_share,
    // so we must destroy our reference to it here
    chunk_destroy(split_key);
  }
}

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
  hbtrie_node_t* root = hbtrie_node_create(btree_node_size);
  if (root == NULL) {
    free(trie);
    return NULL;
  }
  atomic_store(&trie->root, root);

  refcounter_init((refcounter_t*)trie);

  return trie;
}

void hbtrie_destroy(hbtrie_t* trie) {
  if (trie == NULL) return;

  refcounter_dereference((refcounter_t*)trie);
  if (refcounter_count((refcounter_t*)trie) == 0) {
    hbtrie_node_t* root = atomic_load(&trie->root);
    if (root != NULL) {
      hbtrie_node_destroy(root);
    }
    refcounter_destroy_lock((refcounter_t*)trie);
    free(trie);
  }
}

hbtrie_node_t* hbtrie_node_create(uint32_t btree_node_size) {
  hbtrie_node_t* node = memory_pool_alloc(sizeof(hbtrie_node_t));

  node->btree = bnode_create(btree_node_size);
  if (node->btree == NULL) {
    memory_pool_free(node, sizeof(hbtrie_node_t));
    return NULL;
  }
  node->btree_height = 1;  // Single leaf bnode

  // Initialize storage tracking (in-memory by default)
  node->storage = NULL;          // NULL = in-memory only
  node->data_size = 0;           // 0 = not in section
  node->is_loaded = 1;           // Newly created nodes are in memory
  node->is_dirty = 0;            // Not modified yet

  atomic_init(&node->seq, 0);
  platform_lock_init(&node->write_lock);
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
        // Walk the entire bnode tree to find all child hbtrie_node pointers
        vec_t(bnode_t*) bnode_stack;
        vec_init(&bnode_stack);
        vec_push(&bnode_stack, current->btree);

        while (bnode_stack.length > 0) {
          bnode_t* bn = vec_pop(&bnode_stack);
          for (size_t j = 0; j < bnode_count(bn); j++) {
            bnode_entry_t* entry = bnode_get(bn, j);
            if (entry == NULL) continue;

            if (entry->is_bnode_child && entry->child_bnode != NULL) {
              // Internal bnode child - add to bnode stack for traversal
              vec_push(&bnode_stack, entry->child_bnode);
            } else if (!entry->has_value && entry->child != NULL) {
              // Child hbtrie_node - add to node collection
              vec_push(&nodes, entry->child);
            }
          }
        }

        vec_deinit(&bnode_stack);
      }
    }

    // Destroy nodes in reverse order (bottom-up: children before parents)
    for (int i = nodes.length - 1; i >= 0; i--) {
      hbtrie_node_t* current = nodes.data[i];

      // Deallocate from section storage if this node was persisted
      if (current->storage != NULL && current->section_id != 0) {
        sections_deallocate(current->storage, current->section_id,
                            current->block_index, current->data_size);
      }

      if (current->btree != NULL) {
        bnode_destroy_tree(current->btree);
      }
      platform_lock_destroy(&current->write_lock);
      refcounter_destroy_lock((refcounter_t*)current);
      memory_pool_free(current, sizeof(hbtrie_node_t));
    }

    vec_deinit(&nodes);
  }
}


// Deep-copy a bnode tree recursively.
// Copies all bnodes and their entries, following child_bnode pointers
// for internal bnodes and child hbtrie_node pointers for trie descent.
static bnode_t* bnode_tree_copy(bnode_t* root) {
  if (root == NULL) return NULL;

  bnode_t* copy = bnode_create_with_level(root->node_size, atomic_load(&root->level));
  if (copy == NULL) return NULL;

  for (int i = 0; i < root->entries.length; i++) {
    bnode_entry_t* entry = &root->entries.data[i];
    bnode_entry_t new_entry = {0};

    bnode_entry_set_key(&new_entry, bnode_entry_get_key(entry));
    new_entry.has_value = entry->has_value;
    new_entry.is_bnode_child = entry->is_bnode_child;
    new_entry.has_versions = entry->has_versions;

    if (entry->is_bnode_child && entry->child_bnode != NULL) {
      // Internal bnode child - deep copy the subtree
      new_entry.child_bnode = bnode_tree_copy(entry->child_bnode);
    } else if (entry->has_value) {
      if (entry->has_versions && entry->versions != NULL) {
        // Copy version chain
        version_entry_t** tail = &new_entry.versions;
        version_entry_t* src = entry->versions;
        while (src != NULL) {
          version_entry_t* vcopy = version_entry_create(src->txn_id,
              src->value != NULL ? (identifier_t*)refcounter_reference((refcounter_t*)src->value) : NULL,
              src->is_deleted);
          if (vcopy == NULL) {
            // Clean up on failure - version chain partial copy
            version_entry_destroy(new_entry.versions);
            bnode_destroy_tree(copy);
            return NULL;
          }
          *tail = vcopy;
          tail = &vcopy->next;
          src = src->next;
        }
      } else {
        new_entry.value = (identifier_t*)refcounter_reference((refcounter_t*)entry->value);
        new_entry.value_txn_id = entry->value_txn_id;
      }
      // Copy path chunk counts
      if (entry->path_chunk_counts.data != NULL && entry->path_chunk_counts.length > 0) {
        bnode_entry_set_path_chunk_counts(&new_entry,
            entry->path_chunk_counts.data,
            (size_t)entry->path_chunk_counts.length);
      }
    } else if (entry->child != NULL) {
      // Child hbtrie_node - copy handled at the hbtrie level
      // Set to NULL here; caller will fill in via hbtrie_node_copy
      new_entry.child = NULL;
    }

    bnode_insert(copy, &new_entry);
  }

  return copy;
}

hbtrie_node_t* hbtrie_node_copy(hbtrie_node_t* node) {
  if (node == NULL) return NULL;

  hbtrie_node_t* copy = hbtrie_node_create(node->btree->node_size);
  if (copy == NULL) return NULL;
  copy->btree_height = node->btree_height;

  // Deep-copy the bnode tree
  bnode_destroy(copy->btree);
  copy->btree = bnode_tree_copy(node->btree);
  if (copy->btree == NULL) {
    platform_lock_destroy(&copy->write_lock);
    refcounter_destroy_lock((refcounter_t*)copy);
    free(copy);
    return NULL;
  }

  // Walk the copied btree to set child hbtrie_node pointers
  // by recursively copying from the source
  vec_t(bnode_t*) bnode_stack;
  vec_init(&bnode_stack);
  vec_push(&bnode_stack, node->btree);
  vec_push(&bnode_stack, copy->btree);

  while (bnode_stack.length >= 2) {
    bnode_t* src_bn = vec_pop(&bnode_stack);
    bnode_t* dst_bn = vec_pop(&bnode_stack);

    for (size_t i = 0; i < src_bn->entries.length; i++) {
      bnode_entry_t* src_entry = &src_bn->entries.data[i];
      bnode_entry_t* dst_entry = &dst_bn->entries.data[i];

      if (src_entry->is_bnode_child && src_entry->child_bnode != NULL) {
        // Push child bnodes for traversal
        vec_push(&bnode_stack, src_entry->child_bnode);
        vec_push(&bnode_stack, dst_entry->child_bnode);
      } else if (!src_entry->has_value && src_entry->child != NULL) {
        // Recursively copy the child hbtrie_node
        dst_entry->child = hbtrie_node_copy(src_entry->child);
      }
    }
  }

  vec_deinit(&bnode_stack);

  // Copy storage metadata
  copy->storage = node->storage;
  copy->section_id = node->section_id;
  copy->block_index = node->block_index;
  copy->data_size = node->data_size;
  copy->is_loaded = node->is_loaded;
  copy->is_dirty = node->is_dirty;

  return copy;
}

hbtrie_t* hbtrie_copy(hbtrie_t* trie) {
  if (trie == NULL) return NULL;

  hbtrie_t* copy = get_clear_memory(sizeof(hbtrie_t));
  if (copy == NULL) return NULL;

  copy->chunk_size = trie->chunk_size;
  copy->btree_node_size = trie->btree_node_size;

  hbtrie_node_t* src_root = atomic_load(&trie->root);
  if (src_root != NULL) {
    atomic_store(&copy->root, hbtrie_node_copy(src_root));
    if (atomic_load(&copy->root) == NULL) {
      free(copy);
      return NULL;
    }
  }

  refcounter_init((refcounter_t*)copy);

  return copy;
}

void hbtrie_cursor_init(hbtrie_cursor_t* cursor, hbtrie_t* trie, path_t* path) {
  if (cursor == NULL || trie == NULL) return;

  cursor->trie = trie;
  cursor->stack_depth = 0;
  cursor->finished = 0;

  // Push root node onto stack
  hbtrie_node_t* root = atomic_load(&trie->root);
  if (root != NULL) {
    cursor->stack[0].node = root;
    cursor->stack[0].entry_index = 0;
    cursor->stack_depth = 1;
  } else {
    cursor->finished = 1;
  }

  (void)path; // Path is for future seek support
}

hbtrie_cursor_t* hbtrie_cursor_create(hbtrie_t* trie, path_t* path) {
  hbtrie_cursor_t* cursor = get_clear_memory(sizeof(hbtrie_cursor_t));
  if (cursor == NULL) return NULL;
  hbtrie_cursor_init(cursor, trie, path);
  return cursor;
}

void hbtrie_cursor_destroy(hbtrie_cursor_t* cursor) {
  if (cursor == NULL) return;
  // Stack nodes are referenced by the trie; no dereference needed here
  free(cursor);
}

int hbtrie_cursor_next(hbtrie_cursor_t* cursor) {
  if (cursor == NULL || cursor->finished) return -1;

  while (cursor->stack_depth > 0) {
    hbtrie_cursor_frame_t* frame = &cursor->stack[cursor->stack_depth - 1];
    hbtrie_node_t* node = frame->node;

    if (node == NULL || node->btree == NULL) {
      // Empty node, pop and continue
      cursor->stack_depth--;
      continue;
    }

    bnode_t* btree = node->btree;
    size_t count = bnode_count(btree);

    while (frame->entry_index < count) {
      bnode_entry_t* entry = bnode_get(btree, frame->entry_index);
      frame->entry_index++;

      if (entry == NULL) continue;

      if (entry->has_value) {
        // Found an entry with a value — return it
        return 0;
      }

      if (entry->child != NULL && cursor->stack_depth < HBTRIE_CURSOR_MAX_DEPTH) {
        // Descend into child node
        cursor->stack[cursor->stack_depth].node = entry->child;
        cursor->stack[cursor->stack_depth].entry_index = 0;
        cursor->stack_depth++;
        // Will process child on next outer loop iteration
        break;
      }
      // No value and no child — skip
    }

    // If inner loop didn't push a child, pop this frame
    if (frame->entry_index >= count) {
      cursor->stack_depth--;
    }
  }

  cursor->finished = 1;
  return -1;
}

int hbtrie_cursor_at_end(hbtrie_cursor_t* cursor) {
  if (cursor == NULL) return 1;
  return cursor->finished;
}

bnode_entry_t* hbtrie_cursor_get_entry(hbtrie_cursor_t* cursor) {
  if (cursor == NULL || cursor->stack_depth == 0 || cursor->finished) return NULL;

  hbtrie_cursor_frame_t* frame = &cursor->stack[cursor->stack_depth - 1];
  if (frame->entry_index == 0) return NULL;

  // entry_index was already advanced by next(), so look at previous
  bnode_entry_t* entry = bnode_get(frame->node->btree, frame->entry_index - 1);
  return entry;
}

hbtrie_node_t* hbtrie_cursor_get_node(hbtrie_cursor_t* cursor) {
  if (cursor == NULL || cursor->stack_depth == 0) return NULL;
  return cursor->stack[cursor->stack_depth - 1].node;
}

// Forward declaration for recursive serialization
static cbor_item_t* hbtrie_node_to_cbor(hbtrie_node_t* node);
static hbtrie_node_t* cbor_to_hbtrie_node(cbor_item_t* item, uint32_t btree_node_size);
static bnode_t* cbor_to_bnode(cbor_item_t* item, uint32_t btree_node_size);

// Helper: serialize a bnode tree to CBOR.
// For a single-level bnode (leaf), serializes entries directly.
// For multi-level bnodes, serializes each entry with is_bnode_child flag
// and recursively serializes child bnodes.
static cbor_item_t* bnode_to_cbor(bnode_t* root) {
  if (root == NULL) return cbor_new_null();

  // Create array: [level, [[key, has_value, is_bnode_child, value_or_child], ...]]
  cbor_item_t* result = cbor_new_definite_array(2);
  if (result == NULL) return NULL;

  // Level
  cbor_item_t* level = cbor_build_uint16(atomic_load(&root->level));
  cbor_array_push(result, level);
  cbor_decref(&level);

  // Entries array
  cbor_item_t* entries = cbor_new_definite_array((size_t)root->entries.length);
  if (entries == NULL) {
    cbor_decref(&result);
    return NULL;
  }

  for (int i = 0; i < root->entries.length; i++) {
    bnode_entry_t* entry = &root->entries.data[i];
    // [key_bstr, has_value, is_bnode_child, value_or_child_or_bnode]
    cbor_item_t* entry_item = cbor_new_definite_array(4);
    if (entry_item == NULL) {
      cbor_decref(&entries);
      cbor_decref(&result);
      return NULL;
    }

    // Key as byte string
    chunk_t* key = bnode_entry_get_key(entry);
    cbor_item_t* key_bstr = cbor_build_bytestring(
        chunk_data_const(key), key->size);
    cbor_array_push(entry_item, key_bstr);
    cbor_decref(&key_bstr);

    // has_value flag
    cbor_item_t* has_value = entry->has_value ? cbor_build_bool(true) : cbor_build_bool(false);
    cbor_array_push(entry_item, has_value);
    cbor_decref(&has_value);

    // is_bnode_child flag
    cbor_item_t* is_bnode_child = entry->is_bnode_child ? cbor_build_bool(true) : cbor_build_bool(false);
    cbor_array_push(entry_item, is_bnode_child);
    cbor_decref(&is_bnode_child);

    // Value, version chain, child hbtrie_node, or child bnode
    if (entry->has_value) {
      if (entry->has_versions) {
        // MVCC: Serialize version chain
        size_t version_count = 0;
        version_entry_t* current = entry->versions;
        while (current != NULL) {
          version_count++;
          current = current->next;
        }

        cbor_item_t* versions_array = cbor_new_definite_array(version_count);
        if (versions_array == NULL) {
          cbor_decref(&entries);
          cbor_decref(&entry_item);
          cbor_decref(&result);
          return NULL;
        }

        current = entry->versions;
        while (current != NULL) {
          cbor_item_t* version_item = cbor_new_definite_array(3);
          if (version_item == NULL) {
            cbor_decref(&versions_array);
            cbor_decref(&entries);
            cbor_decref(&entry_item);
            cbor_decref(&result);
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
    } else if (entry->is_bnode_child && entry->child_bnode != NULL) {
      // Internal bnode child - serialize recursively
      cbor_item_t* child_cbor = bnode_to_cbor(entry->child_bnode);
      cbor_array_push(entry_item, child_cbor);
      cbor_decref(&child_cbor);
    } else if (entry->child != NULL) {
      // Child hbtrie_node
      cbor_item_t* child_cbor = hbtrie_node_to_cbor(entry->child);
      cbor_array_push(entry_item, child_cbor);
      cbor_decref(&child_cbor);
    } else {
      // Null child (serialized as null)
      cbor_item_t* null_val = cbor_new_null();
      cbor_array_push(entry_item, null_val);
      cbor_decref(&null_val);
    }

    cbor_array_push(entries, entry_item);
    cbor_decref(&entry_item);
  }

  cbor_array_push(result, entries);
  cbor_decref(&entries);
  return result;
}

static cbor_item_t* hbtrie_node_to_cbor(hbtrie_node_t* node) {
  if (node == NULL) {
    return cbor_new_null();
  }

  // Serialize as map: { "btree_height": N, "btree": [...], "entries": [...] }
  // For backward compatibility, we also include the btree_height
  cbor_item_t* map = cbor_new_definite_map(2);
  if (map == NULL) return NULL;

  // btree_height
  cbor_item_t* height = cbor_build_uint16(node->btree_height);
  cbor_item_t* height_key = cbor_build_string("btree_height");
  cbor_map_add(map, (struct cbor_pair){
      .key = height_key,
      .value = height
  });
  cbor_decref(&height_key);
  cbor_decref(&height);

  // btree (serialized as bnode tree)
  cbor_item_t* btree_cbor = bnode_to_cbor(node->btree);
  cbor_item_t* btree_key = cbor_build_string("btree");
  cbor_map_add(map, (struct cbor_pair){
      .key = btree_key,
      .value = btree_cbor
  });
  cbor_decref(&btree_key);
  cbor_decref(&btree_cbor);

  return map;
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

// Deserialize a bnode tree from CBOR.
// New format: [level, [[key, has_value, is_bnode_child, value_or_child], ...]]
// Recursively deserializes internal bnode children (bounded by btree height).
static bnode_t* cbor_to_bnode(cbor_item_t* item, uint32_t btree_node_size) {
  if (item == NULL || cbor_is_null(item)) return NULL;

  if (!cbor_isa_array(item) || cbor_array_size(item) != 2) return NULL;

  // Level
  cbor_item_t* level_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(level_item)) {
    cbor_decref(&level_item);
    return NULL;
  }
  uint16_t level = (uint16_t)cbor_get_int(level_item);
  cbor_decref(&level_item);

  // Entries array
  cbor_item_t* entries_item = cbor_array_get(item, 1);
  if (!cbor_isa_array(entries_item)) {
    cbor_decref(&entries_item);
    return NULL;
  }

  bnode_t* node = bnode_create_with_level(btree_node_size, level);
  if (node == NULL) {
    cbor_decref(&entries_item);
    return NULL;
  }

  size_t num_entries = cbor_array_size(entries_item);
  for (size_t i = 0; i < num_entries; i++) {
    cbor_item_t* entry_item = cbor_array_get(entries_item, i);
    // Accept both old format (3 elements) and new format (4 elements)
    size_t entry_size = cbor_isa_array(entry_item) ? cbor_array_size(entry_item) : 0;
    if (entry_size < 3 || entry_size > 4) {
      cbor_decref(&entry_item);
      cbor_decref(&entries_item);
      bnode_destroy_tree(node);
      return NULL;
    }

    bnode_entry_t entry = {0};

    // Key
    cbor_item_t* key_item = cbor_array_get(entry_item, 0);
    if (!cbor_isa_bytestring(key_item)) {
      cbor_decref(&key_item);
      cbor_decref(&entry_item);
      cbor_decref(&entries_item);
      bnode_destroy_tree(node);
      return NULL;
    }
    chunk_t* chunk_key = chunk_create(cbor_bytestring_handle(key_item), cbor_bytestring_length(key_item));
    cbor_decref(&key_item);
    if (chunk_key == NULL) {
      cbor_decref(&entry_item);
      cbor_decref(&entries_item);
      bnode_destroy_tree(node);
      return NULL;
    }
    bnode_entry_set_key(&entry, chunk_key);
    chunk_destroy(chunk_key);  // set_key shares the reference

    // has_value
    cbor_item_t* has_value_item = cbor_array_get(entry_item, 1);
    entry.has_value = cbor_is_bool(has_value_item) && cbor_get_bool(has_value_item);
    cbor_decref(&has_value_item);

    // is_bnode_child (4th element in new format, defaults to 0 in old format)
    if (entry_size == 4) {
      cbor_item_t* is_bnode_child_item = cbor_array_get(entry_item, 2);
      entry.is_bnode_child = cbor_is_bool(is_bnode_child_item) && cbor_get_bool(is_bnode_child_item);
      cbor_decref(&is_bnode_child_item);
    }

    // Value or child (index 2 in old format, index 3 in new format)
    size_t value_index = (entry_size == 4) ? 3 : 2;
    cbor_item_t* value_or_child = cbor_array_get(entry_item, value_index);

    if (entry.has_value) {
      if (cbor_isa_array(value_or_child) && !cbor_isa_bytestring(value_or_child)) {
        // Check if it's a version chain or a single identifier
        // Version chains are arrays of arrays: [[txn_id, is_deleted, value], ...]
        // Single identifiers serialized by identifier_to_cbor could be bytestrings or arrays
        // We check the first element to distinguish
        if (cbor_array_size(value_or_child) > 0) {
          cbor_item_t* first = cbor_array_get(value_or_child, 0);
          int is_version_chain = cbor_isa_array(first);
          cbor_decref(&first);

          if (is_version_chain) {
            // MVCC: Deserialize version chain
            entry.has_versions = 1;
            entry.versions = NULL;
            version_entry_t* prev_version = NULL;

            size_t num_versions = cbor_array_size(value_or_child);
            for (size_t j = 0; j < num_versions; j++) {
              cbor_item_t* version_item = cbor_array_get(value_or_child, j);
              if (!cbor_isa_array(version_item) || cbor_array_size(version_item) != 3) {
                cbor_decref(&version_item);
                cbor_decref(&value_or_child);
                cbor_decref(&entry_item);
                cbor_decref(&entries_item);
                bnode_destroy_tree(node);
                return NULL;
              }

              cbor_item_t* txn_id_item = cbor_array_get(version_item, 0);
              if (!cbor_isa_array(txn_id_item) || cbor_array_size(txn_id_item) != 3) {
                cbor_decref(&txn_id_item);
                cbor_decref(&version_item);
                cbor_decref(&value_or_child);
                cbor_decref(&entry_item);
                cbor_decref(&entries_item);
                bnode_destroy_tree(node);
                return NULL;
              }

              cbor_item_t* time_item = cbor_array_get(txn_id_item, 0);
              cbor_item_t* nanos_item = cbor_array_get(txn_id_item, 1);
              cbor_item_t* counter_item = cbor_array_get(txn_id_item, 2);

              transaction_id_t txn_id;
              txn_id.time = cbor_isa_uint(time_item) ? cbor_get_uint64(time_item) : 0;
              txn_id.nanos = cbor_isa_uint(nanos_item) ? cbor_get_uint64(nanos_item) : 0;
              txn_id.count = cbor_isa_uint(counter_item) ? cbor_get_uint64(counter_item) : 0;
              cbor_decref(&time_item);
              cbor_decref(&nanos_item);
              cbor_decref(&counter_item);
              cbor_decref(&txn_id_item);

              cbor_item_t* is_deleted_item = cbor_array_get(version_item, 1);
              uint8_t is_deleted = cbor_is_bool(is_deleted_item) && cbor_get_bool(is_deleted_item);
              cbor_decref(&is_deleted_item);

              cbor_item_t* val_item = cbor_array_get(version_item, 2);
              identifier_t* val = NULL;
              if (!cbor_is_null(val_item)) {
                val = cbor_to_identifier(val_item, DEFAULT_CHUNK_SIZE);
              }
              cbor_decref(&val_item);

              version_entry_t* version = version_entry_create(txn_id, val, is_deleted);
              if (version == NULL) {
                if (val != NULL) identifier_destroy(val);
                cbor_decref(&version_item);
                cbor_decref(&value_or_child);
                cbor_decref(&entry_item);
                cbor_decref(&entries_item);
                bnode_destroy_tree(node);
                return NULL;
              }

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
            // Legacy single value in array format
            entry.has_versions = 0;
            entry.value = cbor_to_identifier(value_or_child, DEFAULT_CHUNK_SIZE);
          }
        } else {
          // Empty array - treat as null value
          entry.has_versions = 0;
          entry.value = NULL;
        }
      } else {
        // Legacy: single value (bytestring)
        entry.has_versions = 0;
        entry.value = cbor_to_identifier(value_or_child, DEFAULT_CHUNK_SIZE);
      }
    } else if (entry.is_bnode_child) {
      // Internal bnode child - deserialize recursively
      entry.child_bnode = cbor_to_bnode(value_or_child, btree_node_size);
    } else {
      // Child hbtrie_node
      entry.child = cbor_to_hbtrie_node(value_or_child, btree_node_size);
    }
    cbor_decref(&value_or_child);

    bnode_insert(node, &entry);
    cbor_decref(&entry_item);
  }

  cbor_decref(&entries_item);
  return node;
}

static hbtrie_node_t* cbor_to_hbtrie_node(cbor_item_t* item, uint32_t btree_node_size) {
  if (item == NULL || cbor_is_null(item)) {
    return NULL;
  }

  hbtrie_node_t* node = hbtrie_node_create(btree_node_size);
  if (node == NULL) return NULL;

  // Try new format first (map with btree_height and btree)
  if (cbor_isa_map(item)) {
    cbor_item_t* height_item = cbor_map_find_key(item, "btree_height");
    if (height_item != NULL && cbor_isa_uint(height_item)) {
      node->btree_height = (uint16_t)cbor_get_int(height_item);
    }
    // Note: cbor_map_find_key returns borrowed reference, don't decref

    cbor_item_t* btree_item = cbor_map_find_key(item, "btree");
    if (btree_item != NULL && !cbor_is_null(btree_item)) {
      bnode_destroy(node->btree);
      node->btree = cbor_to_bnode(btree_item, btree_node_size);
      if (node->btree == NULL) {
        hbtrie_node_destroy(node);
        return NULL;
      }
    }
    return node;
  }

  // Old format: plain array of entries (backward compatibility)
  if (!cbor_isa_array(item)) {
    hbtrie_node_destroy(node);
    return NULL;
  }

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
    chunk_t* chunk_key2 = chunk_create(cbor_bytestring_handle(key_item), cbor_bytestring_length(key_item));
    cbor_decref(&key_item);
    if (chunk_key2 == NULL) {
      cbor_decref(&entry_item);
      hbtrie_node_destroy(node);
      return NULL;
    }
    bnode_entry_set_key(&entry, chunk_key2);
    chunk_destroy(chunk_key2);  // set_key shares the reference

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
  cbor_item_t* chunk_size_key = cbor_build_string("chunk_size");
  cbor_map_add(root, (struct cbor_pair){
      .key = chunk_size_key,
      .value = chunk_size
  });
  cbor_decref(&chunk_size_key);
  cbor_decref(&chunk_size);

  // btree_node_size
  cbor_item_t* btree_size = cbor_build_uint32(trie->btree_node_size);
  cbor_item_t* btree_size_key = cbor_build_string("btree_node_size");
  cbor_map_add(root, (struct cbor_pair){
      .key = btree_size_key,
      .value = btree_size
  });
  cbor_decref(&btree_size_key);
  cbor_decref(&btree_size);

  // root node
  cbor_item_t* root_node = hbtrie_node_to_cbor(atomic_load(&trie->root));
  cbor_item_t* root_key = cbor_build_string("root");
  cbor_map_add(root, (struct cbor_pair){
      .key = root_key,
      .value = root_node
  });
  cbor_decref(&root_key);
  cbor_decref(&root_node);

  return root;
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
  uint8_t chunk_size = (uint8_t)cbor_get_int(chunk_size_item);

  // Get btree_node_size
  cbor_item_t* btree_size_item = cbor_map_find_key(item, "btree_node_size");
  if (btree_size_item == NULL || !cbor_isa_uint(btree_size_item)) {
    return NULL;
  }
  uint32_t btree_node_size = (uint32_t)cbor_get_int(btree_size_item);

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
    hbtrie_node_t* old_root = atomic_load(&trie->root);
    hbtrie_node_destroy(old_root);
    atomic_store(&trie->root, root_node);
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

identifier_t* hbtrie_find(hbtrie_t* trie, path_t* path, transaction_id_t read_txn_id) {
  if (trie == NULL || path == NULL) {
    return NULL;
  }

  size_t path_len_ids = path_length(path);
  if (path_len_ids == 0) {
    return NULL;
  }

  // Optimistic read with seqlock validation: retry from root if seqlock changes
  for (;;) {
    hbtrie_node_t* current = atomic_load(&trie->root);
    int retry_needed = 0;

    // Traverse through each identifier in the path
    for (size_t i = 0; i < path_len_ids && !retry_needed; i++) {
      identifier_t* identifier = path_get(path, i);
      if (identifier == NULL) {
        return NULL;
      }

      size_t nchunk = identifier_chunk_count(identifier);

      // Traverse through chunks of this identifier
      for (size_t j = 0; j < nchunk && !retry_needed; j++) {
        chunk_t* chunk = identifier_get_chunk(identifier, j);
        if (chunk == NULL) {
          return NULL;
        }

        // Optimistic read of this hbtrie_node with seqlock validation
        uint64_t seq_before = atomic_load(&current->seq);
        if (seq_before & 1) {
          // Writer active, retry from root
          cpu_relax();
          retry_needed = 1;
          break;
        }

        // Acquire fence: ensure we read btree data AFTER reading seq
        atomic_thread_fence(memory_order_acquire);

        size_t index;
        bnode_entry_t* entry = bnode_find_leaf(current->btree, chunk, &index);

        int is_last_chunk = (j == nchunk - 1);
        int is_last_identifier = (i == path_len_ids - 1);

        if (is_last_chunk && is_last_identifier) {
          // Validate seqlock before reading value
          atomic_thread_fence(memory_order_acquire);
          uint64_t seq_after = atomic_load(&current->seq);
          if (seq_after != seq_before) {
            retry_needed = 1;
            break;
          }

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
          if (entry == NULL || entry->child == NULL) {
            return NULL;
          }

          // Validate seqlock before moving to next node
          atomic_thread_fence(memory_order_acquire);
          uint64_t seq_after = atomic_load(&current->seq);
          if (seq_after != seq_before) {
            retry_needed = 1;
            break;
          }

          current = entry->child;
          // Continue to next identifier
        } else {
          // Intermediate chunk within this identifier
          if (entry == NULL || entry->child == NULL) {
            return NULL;
          }

          // Validate seqlock before moving to child node
          atomic_thread_fence(memory_order_acquire);
          uint64_t seq_after = atomic_load(&current->seq);
          if (seq_after != seq_before) {
            retry_needed = 1;
            break;
          }

          current = entry->child;
          // Continue to next chunk in this identifier
        }
      }
    }

    if (!retry_needed) {
      // Traversed entire path without seqlock conflict
      return NULL;
    }
    // seqlock conflict detected, retry from root
  }
}

identifier_t* hbtrie_find_with_txn(hbtrie_t* trie, path_t* path, txn_desc_t* txn) {
  if (txn == NULL) return NULL;
  return hbtrie_find(trie, path, txn->txn_id);
}

int hbtrie_insert(hbtrie_t* trie, path_t* path, identifier_t* value, transaction_id_t txn_id) {
  if (trie == NULL || path == NULL || value == NULL) {
    return -1;
  }

  hbtrie_node_t* current = atomic_load(&trie->root);
  size_t path_len_ids = path_length(path);

  if (path_len_ids == 0) {
    return -1;
  }

  // Track path for node creation and split propagation
  typedef struct { hbtrie_node_t* node; size_t chunk_index; } mvcc_path_item_t;
  vec_t(mvcc_path_item_t) path_stack;
  vec_init(&path_stack);

  // Track chunk counts per identifier for path metadata
  vec_t(size_t) identifier_chunk_counts;
  vec_init(&identifier_chunk_counts);
  vec_reserve(&identifier_chunk_counts, (int)path_len_ids);

  // Acquire write lock on root node and mark as writing
  platform_lock(&current->write_lock);
  atomic_fetch_add(&current->seq, 1);  // seq becomes odd (writing)

  // Traverse/create path
  for (size_t i = 0; i < path_len_ids; i++) {
    identifier_t* identifier = path_get(path, i);
    if (identifier == NULL) {
      atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
      platform_unlock(&current->write_lock);
      vec_deinit(&path_stack);
      vec_deinit(&identifier_chunk_counts);
      return -1;
    }

    size_t nchunk = identifier_chunk_count(identifier);
    vec_push(&identifier_chunk_counts, nchunk);

    for (size_t j = 0; j < nchunk; j++) {
      chunk_t* chunk = identifier_get_chunk(identifier, j);
      if (chunk == NULL) {
        atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
        platform_unlock(&current->write_lock);
        vec_deinit(&path_stack);
        vec_deinit(&identifier_chunk_counts);
        return -1;
      }

      size_t index;
      btree_path_t bnode_path = {0};
      bnode_t* leaf = btree_descend_with_path(current->btree, chunk, &bnode_path);
      bnode_entry_t* entry = bnode_find(leaf, chunk, &index);

      // Track node for split propagation
      if (path_stack.length == 0 || path_stack.data[path_stack.length - 1].node != current) {
        mvcc_path_item_t ps_item = { current, j };
        vec_push(&path_stack, ps_item);
      }

      int is_last_chunk = (j == nchunk - 1);
      int is_last_identifier = (i == path_len_ids - 1);

      if (is_last_chunk && is_last_identifier) {
        // Final position - insert value with version chain
        if (entry == NULL) {
          // Create new entry
          log_info("MVCC: Creating NEW entry for path (txn=%lu.%09lu.%lu)",
                  txn_id.time, txn_id.nanos, txn_id.count);
          bnode_entry_t new_entry = {0};
          bnode_entry_set_key(&new_entry, chunk);
          new_entry.has_value = 1;
          new_entry.has_versions = 0;  // Legacy mode for first value
          new_entry.value = (identifier_t*)refcounter_reference((refcounter_t*)value);
          new_entry.value_txn_id = txn_id;  // Store transaction ID

          if (bnode_insert(leaf, &new_entry) != 0) {
            bnode_entry_destroy_key(&new_entry);
            atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
            platform_unlock(&current->write_lock);
            vec_deinit(&path_stack);
            vec_deinit(&identifier_chunk_counts);
            return -1;
          }

          // Set path chunk counts on the newly inserted entry
          bnode_entry_t* inserted = bnode_get(leaf, index);
          if (inserted != NULL) {
            bnode_entry_set_path_chunk_counts(inserted,
                identifier_chunk_counts.data,
                (size_t)identifier_chunk_counts.length);
          }

          // Check if leaf bnode needs splitting after insert
          btree_split_after_insert(current, leaf, &bnode_path, trie->chunk_size);
        } else {
          log_info("MVCC: Found EXISTING entry for path (has_value=%d, has_versions=%d, current_txn=%lu.%09lu.%lu, new_txn=%lu.%09lu.%lu)",
                  entry->has_value, entry->has_versions,
                  entry->has_versions ? entry->versions->txn_id.time : entry->value_txn_id.time,
                  entry->has_versions ? entry->versions->txn_id.nanos : entry->value_txn_id.nanos,
                  entry->has_versions ? entry->versions->txn_id.count : entry->value_txn_id.count,
                  txn_id.time, txn_id.nanos, txn_id.count);
          // Entry exists - upgrade to version chain or add version
          if (!entry->has_value) {
            // Entry exists but no value - set first value (legacy mode)
            entry->has_value = 1;
            entry->has_versions = 0;
            entry->value = (identifier_t*)refcounter_reference((refcounter_t*)value);
            entry->value_txn_id = txn_id;  // Store transaction ID
            // Set path chunk counts
            bnode_entry_set_path_chunk_counts(entry,
                identifier_chunk_counts.data,
                (size_t)identifier_chunk_counts.length);
            log_info("MVCC: Set first value (legacy mode) with txn_id=%lu.%09lu.%lu",
                    txn_id.time, txn_id.nanos, txn_id.count);
          } else if (entry->has_versions) {
            // Already has version chain - add new version
            log_info("MVCC: Adding to existing version chain");
            identifier_t* new_value_ref = (identifier_t*)refcounter_reference((refcounter_t*)value);
            if (version_entry_add(&entry->versions, txn_id, new_value_ref, 0) != 0) {
              identifier_destroy(new_value_ref);
              atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
              platform_unlock(&current->write_lock);
              vec_deinit(&path_stack);
              vec_deinit(&identifier_chunk_counts);
              return -1;
            }
            // Set path chunk counts
            bnode_entry_set_path_chunk_counts(entry,
                identifier_chunk_counts.data,
                (size_t)identifier_chunk_counts.length);
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
              atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
              platform_unlock(&current->write_lock);
              vec_deinit(&identifier_chunk_counts);
              vec_deinit(&path_stack);
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
              atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
              platform_unlock(&current->write_lock);
              vec_deinit(&identifier_chunk_counts);
              vec_deinit(&path_stack);
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
            atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
            platform_unlock(&current->write_lock);
            vec_deinit(&identifier_chunk_counts);
            vec_deinit(&path_stack);
            return -1;
          }
          child->storage = current->storage;

          bnode_entry_t new_entry = {0};
          bnode_entry_set_key(&new_entry, chunk);
          new_entry.has_value = 0;
          new_entry.child = child;

          if (bnode_insert(leaf, &new_entry) != 0) {
            bnode_entry_destroy_key(&new_entry);
            hbtrie_node_destroy(child);
            atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
            platform_unlock(&current->write_lock);
            vec_deinit(&identifier_chunk_counts);
            vec_deinit(&path_stack);
            return -1;
          }

          // Check if leaf bnode needs splitting after insert
          btree_split_after_insert(current, leaf, &bnode_path, trie->chunk_size);

          // Crab: lock child before unlocking parent
          platform_lock(&child->write_lock);
          atomic_fetch_add(&child->seq, 1);  // child seq odd (writing)
          atomic_fetch_add(&current->seq, 1);  // parent seq even (stable)
          platform_unlock(&current->write_lock);
          current = child;
        } else if (entry->has_value) {
          // Entry exists with a value but we need to descend further.
          // This happens when key1 is stored and key10 (sharing the 'key1'
          // chunk prefix) needs to go deeper. Create a child node and
          // keep the existing value in the entry (has_value stays 1).
          // The traversal logic will check child first for longer keys.
          hbtrie_node_t* child = hbtrie_node_create(trie->btree_node_size);
          if (child == NULL) {
            atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
            platform_unlock(&current->write_lock);
            vec_deinit(&identifier_chunk_counts);
            vec_deinit(&path_stack);
            return -1;
          }
          child->storage = current->storage;

          // Set the child on the entry, keeping has_value = 1
          // This allows both key1 (value) and key10 (descend further) to work.
          entry->child = child;

          // Crab: lock child before unlocking parent
          platform_lock(&child->write_lock);
          atomic_fetch_add(&child->seq, 1);  // child seq odd (writing)
          atomic_fetch_add(&current->seq, 1);  // parent seq even (stable)
          platform_unlock(&current->write_lock);
          current = child;
        } else {
          if (entry->child == NULL) {
            // Child was serialized as null (empty node), create a new one
            hbtrie_node_t* child = hbtrie_node_create(trie->btree_node_size);
            if (child == NULL) {
              atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
              platform_unlock(&current->write_lock);
              vec_deinit(&identifier_chunk_counts);
              vec_deinit(&path_stack);
              return -1;
            }
            child->storage = current->storage;
            entry->child = child;
          }
          // Crab: lock child before unlocking parent
          platform_lock(&entry->child->write_lock);
          atomic_fetch_add(&entry->child->seq, 1);  // child seq odd (writing)
          atomic_fetch_add(&current->seq, 1);  // parent seq even (stable)
          platform_unlock(&current->write_lock);
          current = entry->child;
        }
      } else {
        // Intermediate chunk - move deeper
        if (entry == NULL) {
          hbtrie_node_t* child = hbtrie_node_create(trie->btree_node_size);
          if (child == NULL) {
            atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
            platform_unlock(&current->write_lock);
            vec_deinit(&identifier_chunk_counts);
            vec_deinit(&path_stack);
            return -1;
          }
          child->storage = current->storage;

          bnode_entry_t new_entry = {0};
          bnode_entry_set_key(&new_entry, chunk);
          new_entry.has_value = 0;
          new_entry.child = child;

          if (bnode_insert(leaf, &new_entry) != 0) {
            bnode_entry_destroy_key(&new_entry);
            hbtrie_node_destroy(child);
            atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
            platform_unlock(&current->write_lock);
            vec_deinit(&identifier_chunk_counts);
            vec_deinit(&path_stack);
            return -1;
          }

          // Check if leaf bnode needs splitting after insert
          btree_split_after_insert(current, leaf, &bnode_path, trie->chunk_size);

          // Crab: lock child before unlocking parent
          platform_lock(&child->write_lock);
          atomic_fetch_add(&child->seq, 1);  // child seq odd (writing)
          atomic_fetch_add(&current->seq, 1);  // parent seq even (stable)
          platform_unlock(&current->write_lock);
          current = child;
        } else if (entry->has_value) {
          // Intermediate chunk: entry has a value but we need to descend further.
          // This happens when a shorter key (e.g. "key1") is stored and a longer
          // key sharing the same prefix (e.g. "key10") needs to go deeper.
          // Create a child node while keeping has_value = 1 so the shorter
          // key's value remains accessible at this entry.
          hbtrie_node_t* child = hbtrie_node_create(trie->btree_node_size);
          if (child == NULL) {
            atomic_fetch_add(&current->seq, 1);
            platform_unlock(&current->write_lock);
            vec_deinit(&identifier_chunk_counts);
            vec_deinit(&path_stack);
            return -1;
          }
          child->storage = current->storage;

          entry->child = child;

          // Crab: lock child before unlocking parent
          platform_lock(&child->write_lock);
          atomic_fetch_add(&child->seq, 1);
          atomic_fetch_add(&current->seq, 1);
          platform_unlock(&current->write_lock);
          current = child;
        } else {
          if (entry->child == NULL) {
            // Child was serialized as null (empty node), create a new one
            hbtrie_node_t* child = hbtrie_node_create(trie->btree_node_size);
            if (child == NULL) {
              atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
              platform_unlock(&current->write_lock);
              vec_deinit(&identifier_chunk_counts);
              vec_deinit(&path_stack);
              return -1;
            }
            child->storage = current->storage;
            entry->child = child;
          }
          // Crab: lock child before unlocking parent
          platform_lock(&entry->child->write_lock);
          atomic_fetch_add(&entry->child->seq, 1);  // child seq odd (writing)
          atomic_fetch_add(&current->seq, 1);  // parent seq even (stable)
          platform_unlock(&current->write_lock);
          current = entry->child;
        }
      }
    }
  }

  (void)path_stack;  // Splits are now handled inline after each insert

  atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
  platform_unlock(&current->write_lock);
  vec_deinit(&identifier_chunk_counts);
  vec_deinit(&path_stack);
  return 0;
}

identifier_t* hbtrie_delete(hbtrie_t* trie, path_t* path, transaction_id_t txn_id) {
  if (trie == NULL || path == NULL) {
    return NULL;
  }

  // Traverse to find the entry
  hbtrie_node_t* current = atomic_load(&trie->root);
  size_t path_len_ids = path_length(path);

  if (path_len_ids == 0) {
    return NULL;
  }

  // Acquire write lock on root node and mark as writing
  platform_lock(&current->write_lock);
  atomic_fetch_add(&current->seq, 1);  // seq becomes odd (writing)

  // Navigate to the final position
  for (size_t i = 0; i < path_len_ids; i++) {
    identifier_t* identifier = path_get(path, i);
    if (identifier == NULL) {
      atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
      platform_unlock(&current->write_lock);
      return NULL;
    }

    size_t nchunk = identifier_chunk_count(identifier);

    for (size_t j = 0; j < nchunk; j++) {
      chunk_t* chunk = identifier_get_chunk(identifier, j);
      if (chunk == NULL) {
        atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
        platform_unlock(&current->write_lock);
        return NULL;
      }

      size_t index;
      bnode_entry_t* entry = bnode_find_leaf(current->btree, chunk, &index);

      int is_last_chunk = (j == nchunk - 1);
      int is_last_identifier = (i == path_len_ids - 1);

      if (is_last_chunk && is_last_identifier) {
        // Final position - create tombstone version
        if (entry == NULL || !entry->has_value) {
          // No entry or no value to delete
          atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
          platform_unlock(&current->write_lock);
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
            atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
            platform_unlock(&current->write_lock);
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
              atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
              platform_unlock(&current->write_lock);
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
              atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
              platform_unlock(&current->write_lock);
              return NULL;
            }
          }
        }

        atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
        platform_unlock(&current->write_lock);
        return last_visible;
      } else if (is_last_chunk) {
        // End of identifier - move to next HBTrie level
        if (entry == NULL || entry->child == NULL) {
          atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
          platform_unlock(&current->write_lock);
          return NULL;
        }
        // Crab: lock child before unlocking parent
        platform_lock(&entry->child->write_lock);
        atomic_fetch_add(&entry->child->seq, 1);  // child seq odd (writing)
        atomic_fetch_add(&current->seq, 1);  // parent seq even (stable)
        platform_unlock(&current->write_lock);
        current = entry->child;
      } else {
        // Intermediate chunk
        if (entry == NULL || entry->child == NULL) {
          atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
          platform_unlock(&current->write_lock);
          return NULL;
        }
        // Crab: lock child before unlocking parent
        platform_lock(&entry->child->write_lock);
        atomic_fetch_add(&entry->child->seq, 1);  // child seq odd (writing)
        atomic_fetch_add(&current->seq, 1);  // parent seq even (stable)
        platform_unlock(&current->write_lock);
        current = entry->child;
      }
    }
  }

  atomic_fetch_add(&current->seq, 1);  // seq becomes even (stable)
  platform_unlock(&current->write_lock);
  return NULL;
}

size_t hbtrie_gc(hbtrie_t* trie, transaction_id_t min_active_txn_id) {
  if (trie == NULL) {
    return 0;
  }
  hbtrie_node_t* root = atomic_load(&trie->root);
  if (root == NULL) {
    return 0;
  }

  // Traverse all nodes and clean up version chains
  size_t total_removed = 0;

  // Use a stack for iterative traversal of hbtrie_nodes
  vec_t(hbtrie_node_t*) stack;
  vec_init(&stack);
  vec_push(&stack, root);

  while (stack.length > 0) {
    hbtrie_node_t* node = vec_pop(&stack);
    if (node == NULL) continue;

    // Acquire write lock on this node for GC modifications
    platform_lock(&node->write_lock);
    atomic_fetch_add(&node->seq, 1);  // seq odd (writing)

    // Walk the entire bnode tree to find all leaf entries
    // Use a stack of bnodes for iterative traversal
    vec_t(bnode_t*) bnode_stack;
    vec_init(&bnode_stack);
    vec_push(&bnode_stack, node->btree);

    while (bnode_stack.length > 0) {
      bnode_t* bn = vec_pop(&bnode_stack);

      // Iterate backwards so we can safely remove tombstoned entries
      for (int i = bn->entries.length - 1; i >= 0; i--) {
        bnode_entry_t* entry = &bn->entries.data[i];

        if (entry->is_bnode_child && entry->child_bnode != NULL) {
          // Internal bnode child - descend into it
          vec_push(&bnode_stack, entry->child_bnode);
        } else if (entry->has_value && entry->has_versions) {
          // Leaf entry with version chain - clean up old versions
          size_t removed = version_entry_gc(&entry->versions, min_active_txn_id, node->storage);
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
          } else if (entry->versions != NULL &&
                     entry->versions->next == NULL &&
                     entry->versions->is_deleted &&
                     transaction_id_compare(&entry->versions->txn_id, &min_active_txn_id) < 0) {
            // Single remaining version is a tombstone older than min_active_txn_id
            // Remove this entry entirely
            bnode_entry_t removed = bnode_remove_at(bn, (size_t)i);
            // Clean up the removed entry's resources
            if (removed.key != NULL) {
              chunk_destroy(removed.key);
            }
            if (removed.has_versions && removed.versions != NULL) {
              // Deallocate section storage for all versions in the removed entry
              if (node->storage != NULL) {
                version_entry_t* v = removed.versions;
                while (v != NULL) {
                  if (v->value_section_id != 0) {
                    sections_deallocate(node->storage, v->value_section_id,
                                        v->value_offset, v->value_data_size);
                  }
                  v = v->next;
                }
              }
              version_entry_destroy(removed.versions);
            }
            total_removed++;
          }
        } else if (!entry->has_value && entry->child != NULL) {
          // Child hbtrie_node - add to node stack for trie traversal
          vec_push(&stack, entry->child);
        }
      }
    }

    atomic_fetch_add(&node->seq, 1);  // seq even (stable)
    platform_unlock(&node->write_lock);
    vec_deinit(&bnode_stack);
  }

  vec_deinit(&stack);
  return total_removed;
}
