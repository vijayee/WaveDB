//
// Created by victor on 3/16/26.
// Performance counters implementation
//

#include "perf_counters.h"
#include "allocator.h"
#include <stdio.h>
#include <string.h>

perf_counters_t* perf_counters_create(void) {
    perf_counters_t* counters = get_clear_memory(sizeof(perf_counters_t));
    refcounter_init((refcounter_t*) counters);
    return counters;
}

void perf_counters_destroy(perf_counters_t* counters) {
    refcounter_dereference((refcounter_t*) counters);
    if (refcounter_count((refcounter_t*) counters) == 0) {
        refcounter_destroy_lock((refcounter_t*) counters);
        free(counters);
    }
}

void perf_counters_reset(perf_counters_t* counters) {
    memset(&counters->wal, 0, sizeof(perf_counter_wal_t));
    memset(&counters->transaction, 0, sizeof(perf_counter_transaction_t));
    memset(&counters->memory, 0, sizeof(perf_counter_memory_t));
    memset(&counters->lock, 0, sizeof(perf_counter_lock_t));
}

// WAL counter operations
void perf_counter_wal_write(perf_counters_t* counters, uint64_t bytes, uint64_t time_ns) {
    counters->wal.writes++;
    counters->wal.bytes_written += bytes;
    counters->wal.write_time_ns += time_ns;
}

void perf_counter_wal_fsync(perf_counters_t* counters, uint64_t time_ns) {
    counters->wal.fsyncs++;
    counters->wal.fsync_time_ns += time_ns;
}

void perf_counter_wal_rotation(perf_counters_t* counters) {
    counters->wal.rotations++;
}

void perf_counter_wal_pending(perf_counters_t* counters, uint64_t pending) {
    counters->wal.pending_writes = pending;
}

// Transaction counter operations
void perf_counter_transaction_generate(perf_counters_t* counters, uint64_t time_ns) {
    counters->transaction.generations++;
    counters->transaction.generation_time_ns += time_ns;
}

void perf_counter_transaction_contention(perf_counters_t* counters) {
    counters->transaction.lock_contentions++;
}

void perf_counter_transaction_clock_backward(perf_counters_t* counters) {
    counters->transaction.clock_backward++;
}

// Memory counter operations
void perf_counter_memory_alloc(perf_counters_t* counters, uint64_t bytes) {
    counters->memory.allocations++;
    counters->memory.bytes_allocated += bytes;
    counters->memory.current_usage += bytes;
    if (counters->memory.current_usage > counters->memory.peak_usage) {
        counters->memory.peak_usage = counters->memory.current_usage;
    }
}

void perf_counter_memory_free(perf_counters_t* counters, uint64_t bytes) {
    counters->memory.deallocations++;
    counters->memory.bytes_freed += bytes;
    if (counters->memory.current_usage >= bytes) {
        counters->memory.current_usage -= bytes;
    } else {
        counters->memory.current_usage = 0;  // Prevent underflow
    }
}

// Lock counter operations
void perf_counter_lock_contention(perf_counters_t* counters, uint64_t wait_time_ns) {
    counters->lock.contentions++;
    counters->lock.wait_time_ns += wait_time_ns;
}

void perf_counter_lock_hold(perf_counters_t* counters, uint64_t hold_time_ns) {
    counters->lock.hold_time_ns += hold_time_ns;
}

// Get statistics
void perf_counters_get_statistics(perf_counters_t* counters, perf_statistics_t* stats) {
    memset(stats, 0, sizeof(perf_statistics_t));

    // WAL statistics
    if (counters->wal.writes > 0) {
        stats->avg_write_time_ns = (double)counters->wal.write_time_ns / counters->wal.writes;
        stats->bytes_per_write = (double)counters->wal.bytes_written / counters->wal.writes;
    }

    if (counters->wal.fsyncs > 0) {
        stats->avg_fsync_time_ns = (double)counters->wal.fsync_time_ns / counters->wal.fsyncs;
    }

    // Transaction statistics
    if (counters->transaction.generations > 0) {
        stats->avg_generation_time_ns = (double)counters->transaction.generation_time_ns / counters->transaction.generations;
    }

    // Lock statistics
    if (counters->lock.contentions > 0) {
        stats->avg_lock_wait_time_ns = (double)counters->lock.wait_time_ns / counters->lock.contentions;
    }
}

// Print counters to stdout
void perf_counters_print(perf_counters_t* counters) {
    printf("========================================\n");
    printf("Performance Counters\n");
    printf("========================================\n\n");

    // WAL counters
    printf("WAL:\n");
    printf("  Writes: %lu\n", counters->wal.writes);
    printf("  Fsyncs: %lu\n", counters->wal.fsyncs);
    printf("  Rotations: %lu\n", counters->wal.rotations);
    printf("  Bytes written: %lu (%.2f MB)\n",
           counters->wal.bytes_written,
           counters->wal.bytes_written / (1024.0 * 1024.0));

    if (counters->wal.writes > 0) {
        printf("  Avg write time: %.2f μs\n",
               (double)counters->wal.write_time_ns / counters->wal.writes / 1e3);
        printf("  Avg bytes/write: %.2f bytes\n",
               (double)counters->wal.bytes_written / counters->wal.writes);
    }

    if (counters->wal.fsyncs > 0) {
        printf("  Avg fsync time: %.2f ms\n",
               (double)counters->wal.fsync_time_ns / counters->wal.fsyncs / 1e6);
    }
    printf("\n");

    // Transaction counters
    printf("Transaction ID:\n");
    printf("  Generations: %lu\n", counters->transaction.generations);
    printf("  Lock contentions: %lu\n", counters->transaction.lock_contentions);
    printf("  Clock backward: %lu\n", counters->transaction.clock_backward);

    if (counters->transaction.generations > 0) {
        printf("  Avg generation time: %.2f ns\n",
               (double)counters->transaction.generation_time_ns / counters->transaction.generations);
    }
    printf("\n");

    // Memory counters
    printf("Memory:\n");
    printf("  Allocations: %lu\n", counters->memory.allocations);
    printf("  Deallocations: %lu\n", counters->memory.deallocations);
    printf("  Bytes allocated: %lu (%.2f MB)\n",
           counters->memory.bytes_allocated,
           counters->memory.bytes_allocated / (1024.0 * 1024.0));
    printf("  Bytes freed: %lu (%.2f MB)\n",
           counters->memory.bytes_freed,
           counters->memory.bytes_freed / (1024.0 * 1024.0));
    printf("  Current usage: %lu bytes (%.2f MB)\n",
           counters->memory.current_usage,
           counters->memory.current_usage / (1024.0 * 1024.0));
    printf("  Peak usage: %lu bytes (%.2f MB)\n",
           counters->memory.peak_usage,
           counters->memory.peak_usage / (1024.0 * 1024.0));
    printf("\n");

    // Lock counters
    printf("Lock:\n");
    printf("  Contentions: %lu\n", counters->lock.contentions);

    if (counters->lock.contentions > 0) {
        printf("  Avg wait time: %.2f ns\n",
               (double)counters->lock.wait_time_ns / counters->lock.contentions);
        printf("  Avg hold time: %.2f ns\n",
               (double)counters->lock.hold_time_ns / counters->lock.contentions);
    }
    printf("\n");
    printf("========================================\n\n");
}

// Save counters to file
int perf_counters_save(perf_counters_t* counters, const char* filename) {
    FILE* fp = fopen(filename, "w");
    if (!fp) {
        perror("Failed to open file for writing");
        return -1;
    }

    fprintf(fp, "{\n");

    // WAL counters
    fprintf(fp, "  \"wal\": {\n");
    fprintf(fp, "    \"writes\": %lu,\n", counters->wal.writes);
    fprintf(fp, "    \"fsyncs\": %lu,\n", counters->wal.fsyncs);
    fprintf(fp, "    \"rotations\": %lu,\n", counters->wal.rotations);
    fprintf(fp, "    \"bytes_written\": %lu,\n", counters->wal.bytes_written);
    fprintf(fp, "    \"write_time_ns\": %lu,\n", counters->wal.write_time_ns);
    fprintf(fp, "    \"fsync_time_ns\": %lu,\n", counters->wal.fsync_time_ns);
    fprintf(fp, "    \"pending_writes\": %lu\n", counters->wal.pending_writes);
    fprintf(fp, "  },\n");

    // Transaction counters
    fprintf(fp, "  \"transaction\": {\n");
    fprintf(fp, "    \"generations\": %lu,\n", counters->transaction.generations);
    fprintf(fp, "    \"lock_contentions\": %lu,\n", counters->transaction.lock_contentions);
    fprintf(fp, "    \"generation_time_ns\": %lu,\n", counters->transaction.generation_time_ns);
    fprintf(fp, "    \"clock_backward\": %lu\n", counters->transaction.clock_backward);
    fprintf(fp, "  },\n");

    // Memory counters
    fprintf(fp, "  \"memory\": {\n");
    fprintf(fp, "    \"allocations\": %lu,\n", counters->memory.allocations);
    fprintf(fp, "    \"deallocations\": %lu,\n", counters->memory.deallocations);
    fprintf(fp, "    \"bytes_allocated\": %lu,\n", counters->memory.bytes_allocated);
    fprintf(fp, "    \"bytes_freed\": %lu,\n", counters->memory.bytes_freed);
    fprintf(fp, "    \"current_usage\": %lu,\n", counters->memory.current_usage);
    fprintf(fp, "    \"peak_usage\": %lu\n", counters->memory.peak_usage);
    fprintf(fp, "  },\n");

    // Lock counters
    fprintf(fp, "  \"lock\": {\n");
    fprintf(fp, "    \"contentions\": %lu,\n", counters->lock.contentions);
    fprintf(fp, "    \"wait_time_ns\": %lu,\n", counters->lock.wait_time_ns);
    fprintf(fp, "    \"hold_time_ns\": %lu\n", counters->lock.hold_time_ns);
    fprintf(fp, "  }\n");

    fprintf(fp, "}\n");

    fclose(fp);
    return 0;
}

// Load counters from file
int perf_counters_load(perf_counters_t* counters, const char* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        perror("Failed to open file for reading");
        return -1;
    }

    // Simple JSON parsing (production code should use a proper JSON library)
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "\"writes\":")) {
            sscanf(line, " \"writes\": %lu,", &counters->wal.writes);
        } else if (strstr(line, "\"fsyncs\":")) {
            sscanf(line, " \"fsyncs\": %lu,", &counters->wal.fsyncs);
        } else if (strstr(line, "\"rotations\":")) {
            sscanf(line, " \"rotations\": %lu,", &counters->wal.rotations);
        } else if (strstr(line, "\"bytes_written\":")) {
            sscanf(line, " \"bytes_written\": %lu,", &counters->wal.bytes_written);
        } else if (strstr(line, "\"write_time_ns\":")) {
            sscanf(line, " \"write_time_ns\": %lu,", &counters->wal.write_time_ns);
        } else if (strstr(line, "\"fsync_time_ns\":")) {
            sscanf(line, " \"fsync_time_ns\": %lu,", &counters->wal.fsync_time_ns);
        } else if (strstr(line, "\"pending_writes\":")) {
            sscanf(line, " \"pending_writes\": %lu", &counters->wal.pending_writes);
        } else if (strstr(line, "\"generations\":")) {
            sscanf(line, " \"generations\": %lu,", &counters->transaction.generations);
        } else if (strstr(line, "\"lock_contentions\":")) {
            sscanf(line, " \"lock_contentions\": %lu,", &counters->transaction.lock_contentions);
        } else if (strstr(line, "\"generation_time_ns\":")) {
            sscanf(line, " \"generation_time_ns\": %lu,", &counters->transaction.generation_time_ns);
        } else if (strstr(line, "\"clock_backward\":")) {
            sscanf(line, " \"clock_backward\": %lu", &counters->transaction.clock_backward);
        }
        // ... (would need to parse all fields for full implementation)
    }

    fclose(fp);
    return 0;
}