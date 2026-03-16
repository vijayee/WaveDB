//
// Created by victor on 3/16/26.
// Performance counters infrastructure for tracking metrics
//

#ifndef WAVEDB_PERF_COUNTERS_H
#define WAVEDB_PERF_COUNTERS_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"

#ifdef __cplusplus
extern "C" {
#endif

// Performance counter categories
typedef enum {
    PERF_COUNTER_WAL,           // WAL operations
    PERF_COUNTER_TRANSACTION,   // Transaction ID operations
    PERF_COUNTER_SECTION,       // Section operations
    PERF_COUNTER_MEMORY,        // Memory operations
    PERF_COUNTER_LOCK,          // Lock operations
    PERF_COUNTER_COUNT          // Number of categories
} perf_counter_category_t;

// WAL performance counters
typedef struct {
    uint64_t writes;            // Number of WAL writes
    uint64_t fsyncs;            // Number of fsync operations
    uint64_t rotations;         // Number of WAL rotations
    uint64_t bytes_written;     // Total bytes written
    uint64_t write_time_ns;     // Total write time (nanoseconds)
    uint64_t fsync_time_ns;     // Total fsync time (nanoseconds)
    uint64_t pending_writes;    // Current pending writes (for debounced fsync)
} perf_counter_wal_t;

// Transaction ID performance counters
typedef struct {
    uint64_t generations;       // Number of transaction IDs generated
    uint64_t lock_contentions;  // Number of lock contentions
    uint64_t generation_time_ns;// Total generation time (nanoseconds)
    uint64_t clock_backward;    // Number of clock backward events
} perf_counter_transaction_t;

// Section performance counters
typedef struct {
    uint64_t writes;           // Number of section writes
    uint64_t fragment_scans;    // Number of fragment scans
    uint64_t fragment_count;    // Total fragments checked
    uint64_t metadata_saves;    // Number of metadata saves
    uint64_t checkout_waits;    // Number of checkout lock waits
    uint64_t write_time_ns;     // Total write time (nanoseconds)
    uint64_t scan_time_ns;      // Total fragment scan time (nanoseconds)
    uint64_t metadata_time_ns;  // Total metadata save time (nanoseconds)
} perf_counter_section_t;

// Memory performance counters
typedef struct {
    uint64_t allocations;       // Number of allocations
    uint64_t deallocations;      // Number of deallocations
    uint64_t bytes_allocated;    // Total bytes allocated
    uint64_t bytes_freed;        // Total bytes freed
    uint64_t current_usage;      // Current memory usage
    uint64_t peak_usage;         // Peak memory usage
} perf_counter_memory_t;

// Lock performance counters
typedef struct {
    uint64_t contentions;       // Number of lock contentions
    uint64_t wait_time_ns;       // Total wait time (nanoseconds)
    uint64_t hold_time_ns;        // Total hold time (nanoseconds)
} perf_counter_lock_t;

// Global performance counters structure
typedef struct {
    refcounter_t refcounter;
    perf_counter_wal_t wal;
    perf_counter_transaction_t transaction;
    perf_counter_section_t section;
    perf_counter_memory_t memory;
    perf_counter_lock_t lock;
} perf_counters_t;

// Create performance counters
perf_counters_t* perf_counters_create(void);

// Destroy performance counters
void perf_counters_destroy(perf_counters_t* counters);

// Reset all counters to zero
void perf_counters_reset(perf_counters_t* counters);

// WAL counter operations
void perf_counter_wal_write(perf_counters_t* counters, uint64_t bytes, uint64_t time_ns);
void perf_counter_wal_fsync(perf_counters_t* counters, uint64_t time_ns);
void perf_counter_wal_rotation(perf_counters_t* counters);
void perf_counter_wal_pending(perf_counters_t* counters, uint64_t pending);

// Transaction counter operations
void perf_counter_transaction_generate(perf_counters_t* counters, uint64_t time_ns);
void perf_counter_transaction_contention(perf_counters_t* counters);
void perf_counter_transaction_clock_backward(perf_counters_t* counters);

// Section counter operations
void perf_counter_section_write(perf_counters_t* counters, uint64_t time_ns);
void perf_counter_section_fragment_scan(perf_counters_t* counters, uint64_t fragment_count, uint64_t time_ns);
void perf_counter_section_metadata_save(perf_counters_t* counters, uint64_t time_ns);
void perf_counter_section_checkout_wait(perf_counters_t* counters);

// Memory counter operations
void perf_counter_memory_alloc(perf_counters_t* counters, uint64_t bytes);
void perf_counter_memory_free(perf_counters_t* counters, uint64_t bytes);

// Lock counter operations
void perf_counter_lock_contention(perf_counters_t* counters, uint64_t wait_time_ns);
void perf_counter_lock_hold(perf_counters_t* counters, uint64_t hold_time_ns);

// Get statistics
typedef struct {
    double avg_write_time_ns;
    double avg_fsync_time_ns;
    double avg_fragment_scan_time_ns;
    double avg_metadata_save_time_ns;
    double avg_generation_time_ns;
    double avg_lock_wait_time_ns;
    double avg_lock_hold_time_ns;
    double bytes_per_write;
    double fragments_per_scan;
} perf_statistics_t;

void perf_counters_get_statistics(perf_counters_t* counters, perf_statistics_t* stats);

// Print counters to stdout
void perf_counters_print(perf_counters_t* counters);

// Save counters to file
int perf_counters_save(perf_counters_t* counters, const char* filename);

// Load counters from file
int perf_counters_load(perf_counters_t* counters, const char* filename);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_PERF_COUNTERS_H