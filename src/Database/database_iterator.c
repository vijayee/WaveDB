//
// Database Iterator for HBTrie traversal
//

#include "database_iterator.h"
#include "../HBTrie/chunk.h"
#include "../Buffer/buffer.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

// Initial stack size
#define INITIAL_STACK_SIZE 16

/**
 * Push a frame onto the iterator stack.
 */
static int push_frame(database_iterator_t* iter, hbtrie_node_t* node, size_t path_index) {
    // Grow stack if needed
    if (iter->stack_depth >= iter->stack_size) {
        size_t new_size = iter->stack_size * 2;
        iterator_frame_t* new_stack = realloc(iter->stack,
                                               new_size * sizeof(iterator_frame_t));
        if (new_stack == NULL) {
            return -1;
        }
        iter->stack = new_stack;
        iter->stack_size = new_size;
    }

    // Reference the node
    REFERENCE(node, hbtrie_node_t);

    iter->stack[iter->stack_depth].node = node;
    iter->stack[iter->stack_depth].entry_index = 0;
    iter->stack[iter->stack_depth].path_index = path_index;
    iter->stack_depth++;

    return 0;
}

/**
 * Pop a frame from the iterator stack.
 */
static void pop_frame(database_iterator_t* iter) {
    if (iter->stack_depth > 0) {
        iter->stack_depth--;
        // Dereference the node
        if (iter->stack[iter->stack_depth].node) {
            DEREFERENCE(iter->stack[iter->stack_depth].node);
        }
    }
}

/**
 * Check if a path is within the scan bounds.
 * Returns:
 *   1  if start_path <= path < end_path (within bounds)
 *   0  if path is before start_path (skip, but keep scanning)
 *  -1  if path >= end_path (past upper bound, stop iteration)
 */
static int within_bounds(database_iterator_t* iter, path_t* path) {
    if (path == NULL) return 0;

    // Check upper bound: if path >= end_path, iteration is done
    if (iter->end_path != NULL) {
        if (path_compare(path, iter->end_path) >= 0) {
            return -1;
        }
    }

    // Check lower bound: if path < start_path, skip this entry
    if (iter->start_path != NULL) {
        if (path_compare(path, iter->start_path) < 0) {
            return 0;
        }
    }

    return 1;
}

/**
 * Build an identifier from an array of chunks.
 *
 * Creates an identifier by concatenating chunk data.
 * If byte_length is provided (>0), builds a buffer of exactly that size,
 * copying only the real bytes from each chunk (no null padding). This
 * produces an identifier with the exact original length.
 * If byte_length is 0 or exceeds nchunks*chunk_size, falls back to padded
 * behavior (legacy compatibility).
 */
static identifier_t* build_identifier_from_chunks(chunk_t** chunks, size_t nchunks,
                                                    uint8_t chunk_size, size_t byte_length) {
    if (chunks == NULL || nchunks == 0) {
        return identifier_create_empty(chunk_size);
    }

    // Determine the real data length
    size_t total_padded = nchunks * chunk_size;
    size_t real_length = total_padded;

    // Use byte_length if it's valid (non-zero and within the padded range)
    if (byte_length > 0 && byte_length <= total_padded) {
        real_length = byte_length;
    }

    // Allocate buffer for concatenated data (exact size, no padding)
    buffer_t* buf = buffer_create(real_length);
    if (buf == NULL) {
        return NULL;
    }

    // Copy chunk data, only up to real_length bytes
    size_t offset = 0;
    size_t remaining = real_length;
    for (size_t i = 0; i < nchunks && remaining > 0; i++) {
        if (chunks[i] != NULL) {
            size_t to_copy = (remaining < chunk_size) ? remaining : chunk_size;
            memcpy(buf->data + offset, chunks[i]->data, to_copy);
            offset += to_copy;
            remaining -= to_copy;
        }
    }

    identifier_t* id = identifier_create(buf, chunk_size);
    buffer_destroy(buf);
    return id;
}

database_iterator_t* database_scan_start(database_t* db,
                                          path_t* start_path,
                                          path_t* end_path) {
    if (db == NULL) {
        return NULL;
    }

    database_iterator_t* iter = get_clear_memory(sizeof(database_iterator_t));
    if (iter == NULL) {
        return NULL;
    }

    iter->db = db;
    // Copy paths — the iterator owns its copies, callers retain theirs
    iter->start_path = start_path ? path_copy(start_path) : NULL;
    iter->end_path = end_path ? path_copy(end_path) : NULL;
    iter->current_path = path_create();
    iter->finished = 0;

    // Allocate initial stack
    iter->stack = get_clear_memory(INITIAL_STACK_SIZE * sizeof(iterator_frame_t));
    if (iter->stack == NULL) {
        if (iter->start_path) path_destroy(iter->start_path);
        if (iter->end_path) path_destroy(iter->end_path);
        if (iter->current_path) path_destroy(iter->current_path);
        free(iter);
        return NULL;
    }
    iter->stack_size = INITIAL_STACK_SIZE;
    iter->stack_depth = 0;

    // Initialize read transaction ID from database's transaction manager
    // For synchronous scans, we use the current transaction context
    iter->read_txn_id = tx_manager_get_last_committed(db->tx_manager);

    // Push root node onto stack
    hbtrie_node_t* root = db->trie ? atomic_load_ptr(&db->trie->root, hbtrie_node_t*) : NULL;
    if (root) {
        iter->stack[0].node = root;
        iter->stack[0].entry_index = 0;
        iter->stack[0].path_index = 0;
        iter->stack_depth = 1;

        // Reference the root node
        REFERENCE(root, hbtrie_node_t);
    }

    refcounter_init((refcounter_t*)iter);
    return iter;
}

void database_scan_end(database_iterator_t* iter) {
    if (iter == NULL) return;

    // Dereference the iterator
    refcounter_dereference((refcounter_t*)iter);

    // Check if we should free
    if (refcounter_count((refcounter_t*)iter) == 0) {
        // Clean up paths
        if (iter->start_path) path_destroy(iter->start_path);
        if (iter->end_path) path_destroy(iter->end_path);
        if (iter->current_path) path_destroy(iter->current_path);

        // Dereference all nodes on stack
        for (size_t i = 0; i < iter->stack_depth; i++) {
            if (iter->stack[i].node) {
                DEREFERENCE(iter->stack[i].node);
            }
        }

        // Free stack
        if (iter->stack) free(iter->stack);

        // Free iterator
        free(iter);
    }
}

int database_scan_next(database_iterator_t* iter,
                        path_t** out_path,
                        identifier_t** out_value) {
    if (iter == NULL || out_path == NULL || out_value == NULL) {
        return -2;
    }

    *out_path = NULL;
    *out_value = NULL;

    if (iter->finished || iter->stack_depth == 0) {
        return -1;  // End of iteration
    }

    // Get chunk_size from database trie
    uint8_t chunk_size = iter->db->trie ? iter->db->trie->chunk_size : DEFAULT_CHUNK_SIZE;

    // Depth-first traversal
    while (iter->stack_depth > 0) {
        // Get current frame
        iterator_frame_t* frame = &iter->stack[iter->stack_depth - 1];
        hbtrie_node_t* node = frame->node;

        if (node == NULL || node->btree == NULL) {
            pop_frame(iter);
            continue;
        }

        // Get current entry
        bnode_t* btree = node->btree;
        size_t count = bnode_count(btree);

        // Track if we pushed a child frame (if so, don't pop this frame)
        int pushed_child = 0;

        // Process entries at current level
        while (frame->entry_index < count) {
            bnode_entry_t* entry = bnode_get(btree, frame->entry_index);
            // Don't increment entry_index yet - we may need to push a child

            if (entry == NULL) {
                frame->entry_index++;  // Skip null entries
                continue;
            }

            // Check if this entry has a value (complete path)
            if (entry->has_value) {
                frame->entry_index++;  // Move past this entry since we're processing it

                // If entry also has a trie_child, push it for later traversal
                if (entry->trie_child == NULL && entry->child_disk_offset != 0 && iter->db->trie->fcache != NULL) {
                    bnode_entry_lazy_load_trie_child(entry, iter->db->trie->fcache,
                                                     iter->db->trie->chunk_size,
                                                     iter->db->trie->btree_node_size);
                }
                if (entry->trie_child) {
                    if (push_frame(iter, entry->trie_child, iter->stack_depth - 1) < 0) {
                        return -2;
                    }
                }

                identifier_t* value = NULL;

                // Get visible value from version chain
                if (entry->has_versions && entry->versions) {
                    version_entry_t* visible = version_entry_find_visible(
                        entry->versions, iter->read_txn_id);
                    if (visible && !visible->is_deleted && visible->value) {
                        value = REFERENCE(visible->value, identifier_t);
                    }
                } else if (entry->value) {
                    // Legacy single value
                    value = REFERENCE(entry->value, identifier_t);
                }

                if (value) {
                    // Build path from stack using path_meta metadata
                    // The leaf entry stores per-subscript {chunk_count, byte_length}

                    // Get path metadata from the leaf entry
                    size_t num_identifiers = 0;
                    const path_subscript_meta_t* path_meta = bnode_entry_get_path_meta(entry, &num_identifiers);

                    // Collect chunks from the stack
                    size_t nchunks = 0;
                    for (size_t i = 0; i < iter->stack_depth; i++) {
                        iterator_frame_t* f = &iter->stack[i];
                        if (f->entry_index > 0) {
                            bnode_entry_t* e = bnode_get(f->node->btree, f->entry_index - 1);
                            if (e != NULL && e->key != NULL) {
                                nchunks++;
                            }
                        }
                    }

                    // Allocate chunk array
                    chunk_t** chunks = NULL;
                    if (nchunks > 0) {
                        chunks = malloc(nchunks * sizeof(chunk_t*));
                        if (chunks == NULL) {
                            identifier_destroy(value);
                            return -2;
                        }

                        size_t idx = 0;
                        for (size_t i = 0; i < iter->stack_depth; i++) {
                            iterator_frame_t* f = &iter->stack[i];
                            if (f->entry_index > 0) {
                                bnode_entry_t* e = bnode_get(f->node->btree, f->entry_index - 1);
                                if (e != NULL && e->key != NULL) {
                                    chunks[idx++] = e->key;
                                }
                            }
                        }
                    }

                    // Build path from collected chunks
                    path_t* result_path = path_create();
                    if (result_path == NULL) {
                        if (chunks) free(chunks);
                        identifier_destroy(value);
                        return -2;
                    }

                    if (path_meta != NULL && num_identifiers > 0) {
                        // Use metadata to split chunks into identifiers with exact lengths
                        size_t chunk_offset = 0;
                        // Skip prefix identifiers (subtree prefix components).
                        // prefix_skip is 0 for non-subtree iterators.
                        size_t skip = iter->prefix_skip;
                        if (skip > num_identifiers) skip = num_identifiers;
                        for (size_t i = 0; i < skip; i++) {
                            chunk_offset += path_meta[i].chunk_count;
                        }
                        // Build remaining identifiers (post-prefix)
                        for (size_t i = skip; i < num_identifiers; i++) {
                            size_t id_chunks = path_meta[i].chunk_count;
                            size_t id_byte_length = path_meta[i].byte_length;
                            identifier_t* id = build_identifier_from_chunks(
                                chunks + chunk_offset, id_chunks, chunk_size, id_byte_length);
                            if (id == NULL) {
                                if (chunks) free(chunks);
                                path_destroy(result_path);
                                identifier_destroy(value);
                                return -2;
                            }
                            int rc = path_append(result_path, id);
                            identifier_destroy(id);  // path_append takes a reference
                            if (rc != 0) {
                                if (chunks) free(chunks);
                                path_destroy(result_path);
                                identifier_destroy(value);
                                return -2;
                            }
                            chunk_offset += id_chunks;
                        }
                    } else {
                        // Legacy entry without metadata - treat as single identifier (padded)
                        identifier_t* key_id = build_identifier_from_chunks(chunks, nchunks, chunk_size, 0);
                        if (key_id == NULL) {
                            if (chunks) free(chunks);
                            path_destroy(result_path);
                            identifier_destroy(value);
                            return -2;
                        }
                        int rc = path_append(result_path, key_id);
                        identifier_destroy(key_id);
                        if (rc != 0) {
                            if (chunks) free(chunks);
                            path_destroy(result_path);
                            identifier_destroy(value);
                            return -2;
                        }
                    }

                    if (chunks) free(chunks);

                    // Check bounds on the result path
                    int bounds = within_bounds(iter, result_path);
                    if (bounds < 0) {
                        // Past upper bound — stop iteration entirely
                        path_destroy(result_path);
                        identifier_destroy(value);
                        iter->finished = 1;
                        return -1;
                    }
                    if (bounds == 0) {
                        // Before lower bound — skip this entry, keep scanning
                        path_destroy(result_path);
                        identifier_destroy(value);
                        continue;
                    }

                    *out_path = result_path;
                    *out_value = value;
                    return 0;  // Success
                }
            } else if (entry->child) {
                // Entry has a child node - push it for deeper traversal
                // Increment entry_index before pushing so we don't revisit this entry
                frame->entry_index++;
                pushed_child = 1;  // Mark that we pushed a child
                // Push child frame
                if (push_frame(iter, entry->child, iter->stack_depth - 1) < 0) {
                    return -2;  // Error
                }
                break;  // Will continue with child on next iteration
            } else if (entry->trie_child || entry->child_disk_offset != 0) {
                // Lazy-load trie_child if needed
                if (entry->trie_child == NULL && entry->child_disk_offset != 0 && iter->db->trie->fcache != NULL) {
                    bnode_entry_lazy_load_trie_child(entry, iter->db->trie->fcache,
                                                     iter->db->trie->chunk_size,
                                                     iter->db->trie->btree_node_size);
                }
                if (entry->trie_child == NULL) {
                    frame->entry_index++;
                    continue;
                }
                // Entry has both value and trie_child - push trie_child for traversal
                frame->entry_index++;
                pushed_child = 1;
                if (push_frame(iter, entry->trie_child, iter->stack_depth - 1) < 0) {
                    return -2;
                }
                break;
            } else {
                // Entry has no value and no child - skip it
                frame->entry_index++;
            }
        }

        // If we've processed all entries at this level and didn't push a child, pop the frame
        if (!pushed_child && frame->entry_index >= count) {
            pop_frame(iter);
        }
    }

    // No more entries
    iter->finished = 1;
    return -1;
}