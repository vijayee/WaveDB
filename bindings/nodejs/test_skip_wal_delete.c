// Test if skipping WAL for delete fixes the crash
#include <stdio.h>

// Override get_thread_wal to return NULL for testing
thread_wal_t* get_thread_wal_test(wal_manager_t* manager) {
    fprintf(stderr, "DEBUG: Skipping thread-local WAL for testing\n");
    fflush(stderr);
    return NULL;  // Skip WAL
}

// This would require recompiling database.c with the override
