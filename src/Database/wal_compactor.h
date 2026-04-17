//
// Created by victor on 3/21/26.
//

#ifndef WAVEDB_WAL_COMPACTOR_H
#define WAVEDB_WAL_COMPACTOR_H

#include "wal_manager.h"
#include "../Util/threadding.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compaction thread state
 */
typedef struct {
    wal_manager_t* manager;
    uint64_t last_write_time;
    uint64_t idle_threshold_ms;
    uint64_t compact_interval_ms;
    uint64_t last_compact_time;
    int running;
    PLATFORMTHREADTYPE(thread);
    PLATFORMLOCKTYPE(lock);
    PLATFORMCONDITIONTYPE(cond);
} wal_compactor_t;

/**
 * Create compaction thread
 */
wal_compactor_t* wal_compactor_create(wal_manager_t* manager,
                                       uint64_t idle_threshold_ms,
                                       uint64_t compact_interval_ms);

/**
 * Destroy compaction thread
 */
void wal_compactor_destroy(wal_compactor_t* compactor);

/**
 * Signal write activity (resets idle timer)
 */
void wal_compactor_signal_write(wal_compactor_t* compactor);

/**
 * Force immediate compaction
 */
int wal_compactor_force_compact(wal_compactor_t* compactor);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_WAL_COMPACTOR_H