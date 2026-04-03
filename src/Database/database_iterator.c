//
// Database Iterator for HBTrie traversal
//

#include "database_iterator.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

// Initial stack size
#define INITIAL_STACK_SIZE 16

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