//
// Database Iterator for HBTrie traversal
//

#include "database_iterator.h"
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
        if (new_stack == NULL) return -1;
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
 * Check if we're within bounds (start_path <= current < end_path)
 * TODO: Implement path comparison for bound checking
 * For now, allow all paths
 */
static int within_bounds(database_iterator_t* iter) {
    (void)iter;  // Suppress unused parameter warning
    return 1;
}

database_iterator_t* database_scan_start(database_t* db,
                                          path_t* start_path,
                                          path_t* end_path) {
    if (db == NULL) {
        // Clean up paths if provided
        if (start_path) path_destroy(start_path);
        if (end_path) path_destroy(end_path);
        return NULL;
    }

    database_iterator_t* iter = get_clear_memory(sizeof(database_iterator_t));
    if (iter == NULL) {
        if (start_path) path_destroy(start_path);
        if (end_path) path_destroy(end_path);
        return NULL;
    }

    iter->db = db;
    iter->start_path = start_path;
    iter->end_path = end_path;
    iter->current_path = path_create();
    iter->finished = 0;

    // Allocate initial stack
    iter->stack = get_clear_memory(INITIAL_STACK_SIZE * sizeof(iterator_frame_t));
    if (iter->stack == NULL) {
        if (start_path) path_destroy(start_path);
        if (end_path) path_destroy(end_path);
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
    if (db->trie && db->trie->root) {
        iter->stack[0].node = db->trie->root;
        iter->stack[0].entry_index = 0;
        iter->stack[0].path_index = 0;
        iter->stack_depth = 1;

        // Reference the root node
        REFERENCE(db->trie->root, hbtrie_node_t);
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

        // Destroy refcounter lock and free
        refcounter_destroy_lock((refcounter_t*)iter);
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

        // Process entries at current level
        while (frame->entry_index < count) {
            bnode_entry_t* entry = bnode_get(btree, frame->entry_index);
            frame->entry_index++;

            if (entry == NULL) continue;

            // Check if this entry has a value (leaf node)
            if (entry->has_value) {
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
                    // Build result path
                    *out_path = path_copy(iter->current_path);
                    *out_value = value;

                    if (*out_path == NULL) {
                        identifier_destroy(value);
                        return -2;
                    }

                    return 0;  // Success
                }
            }

            // If entry has a child, push it onto stack for deeper traversal
            if (!entry->has_value && entry->child) {
                // Push child frame
                if (push_frame(iter, entry->child, iter->stack_depth - 1) < 0) {
                    return -2;  // Error
                }
                break;  // Will continue with child on next iteration
            }
        }

        // If we've processed all entries at this level, pop the frame
        if (frame->entry_index >= count) {
            pop_frame(iter);
        }
    }

    // No more entries
    iter->finished = 1;
    return -1;
}