//
// Created by victor on 3/21/26.
//

#include "wal_compactor.h"
#include "../Util/allocator.h"
#include "../Time/debouncer.h"
#include <stdlib.h>
#include <unistd.h>

// Background thread function
static void* compaction_thread_func(void* arg) {
    wal_compactor_t* compactor = (wal_compactor_t*)arg;

    while (1) {
        sleep(1);  // Check every second

        platform_lock(&compactor->lock);

        // Check if we should stop
        if (!compactor->running) {
            platform_unlock(&compactor->lock);
            break;
        }

        uint64_t now = 0;
        timeval_t tv;
        get_time(&tv);
        now = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;

        uint64_t time_since_last_write = now - compactor->last_write_time;
        uint64_t time_since_last_compact = now - compactor->last_compact_time;

        platform_unlock(&compactor->lock);

        // Trigger if idle or interval elapsed
        int should_compact = 0;
        if (time_since_last_write > compactor->idle_threshold_ms) {
            should_compact = 1;
        } else if (time_since_last_compact > compactor->compact_interval_ms) {
            should_compact = 1;
        }

        if (should_compact) {
            compact_wal_files(compactor->manager);
            compactor->last_compact_time = now;
        }
    }

    return NULL;
}

wal_compactor_t* wal_compactor_create(wal_manager_t* manager,
                                       uint64_t idle_threshold_ms,
                                       uint64_t compact_interval_ms) {
    wal_compactor_t* compactor = get_clear_memory(sizeof(wal_compactor_t));
    if (compactor == NULL) {
        return NULL;
    }

    compactor->manager = manager;
    compactor->idle_threshold_ms = idle_threshold_ms;
    compactor->compact_interval_ms = compact_interval_ms;

    timeval_t now;
    get_time(&now);
    compactor->last_write_time = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_usec / 1000;
    compactor->last_compact_time = 0;
    compactor->running = 1;

    platform_lock_init(&compactor->lock);
    platform_condition_init(&compactor->cond);

    // Create thread
    if (pthread_create(&compactor->thread, NULL, compaction_thread_func, compactor) != 0) {
        free(compactor);
        return NULL;
    }

    return compactor;
}

void wal_compactor_destroy(wal_compactor_t* compactor) {
    if (compactor == NULL) return;

    // Signal thread to stop
    compactor->running = 0;

    // Wait for thread to finish (it will complete its current sleep cycle)
    pthread_join(compactor->thread, NULL);

    // Now safe to destroy locks - thread has exited and released all locks
    platform_lock_destroy(&compactor->lock);
    platform_condition_destroy(&compactor->cond);

    free(compactor);
}

void wal_compactor_signal_write(wal_compactor_t* compactor) {
    platform_lock(&compactor->lock);
    timeval_t now;
    get_time(&now);
    compactor->last_write_time = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_usec / 1000;
    platform_unlock(&compactor->lock);
}

int wal_compactor_force_compact(wal_compactor_t* compactor) {
    return compact_wal_files(compactor->manager);
}