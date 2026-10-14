//
// Cross-process database file lock — see db_file_lock.c for rationale.
//

#ifndef WAVEDB_UTIL_DB_FILE_LOCK_H
#define WAVEDB_UTIL_DB_FILE_LOCK_H

#include <stdint.h>

#if _WIN32
#include "Util/windows_compat.h"
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define DB_FILE_LOCK_NAME "db.wdbl"

// Error codes (superset-safe: negative, distinct from -100..-102)
#define DB_FILE_LOCK_OK          0
#define DB_FILE_LOCK_HELD        -1
#define DB_FILE_LOCK_ERROR       -2
#define DB_FILE_LOCK_INVALID_ARG -3

int db_file_lock_acquire(const char* location, void** out_handle);
void db_file_lock_release(void** handle);

#endif  // WAVEDB_UTIL_DB_FILE_LOCK_H