//
// Cross-process database file lock.
//
// WaveDB is single-writer by design: the WAL/section persistence state lives
// per-handle, and a second open of the same directory rewinds data at its
// close-time persist/compaction (observed as silent tail loss of every record
// written before the second handle had opened). The fix is fail-fast: the
// first open of a directory owns an OS-level lock file held for the handle's
// lifetime; any other open fails DATABASE_LOCKED instead of silently
// corrupting.
//
// Semantics:
//   - Windows: the lock IS the OS file handle, opened with share mode 0. A
//     second open by any process fails (sharing violation), and a dead owner
//     releases the lock automatically (no stale-lock recovery needed).
//   - POSIX: flock(LOCK_EX | LOCK_NB) on a lock file descriptor; flock locks
//     are bound to the open file description and die with the process.
//   - The lock file's CONTENT is never used; existence alone is harmless,
//     so we do not delete it on release (avoids the recreate race).
//

#include "db_file_lock.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "log.h"
#include "path_join.h"

int db_file_lock_acquire(const char* location, void** out_handle) {
    if (location == NULL || out_handle == NULL) {
        return DB_FILE_LOCK_INVALID_ARG;
    }
    *out_handle = NULL;

    char lock_path[1024];
    char* joined = path_join((char*)location, DB_FILE_LOCK_NAME);
    if (joined == NULL) {
        log_warn("db_file_lock: path_join failed for %s", location);
        return DB_FILE_LOCK_ERROR;
    }
    snprintf(lock_path, sizeof(lock_path), "%s", joined);
    free(joined);

#if _WIN32
    HANDLE handle = CreateFileA(lock_path,
                                GENERIC_WRITE,
                                0,              /* share mode 0: exclusive */
                                NULL,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL,
                                NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
            log_info("db_file_lock: %s is locked by another process", lock_path);
            return DB_FILE_LOCK_HELD;
        }
        log_warn("db_file_lock: CreateFileA failed on %s (err=%lu)",
                 lock_path, (unsigned long)err);
        return DB_FILE_LOCK_ERROR;
    }
    *out_handle = (void*)handle;
    return DB_FILE_LOCK_OK;
#else
    int fd = open(lock_path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        log_warn("db_file_lock: open failed on %s (errno=%d)", lock_path, errno);
        return DB_FILE_LOCK_ERROR;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved = errno;
        close(fd);
        if (saved == EWOULDBLOCK) {
            log_info("db_file_lock: %s is locked by another process", lock_path);
            return DB_FILE_LOCK_HELD;
        }
        log_warn("db_file_lock: flock failed on %s (errno=%d)", lock_path, saved);
        return DB_FILE_LOCK_ERROR;
    }
    int* boxed = (int*)malloc(sizeof(int));
    if (boxed == NULL) {
        close(fd);
        return DB_FILE_LOCK_ERROR;
    }
    *boxed = fd;
    *out_handle = (void*)boxed;
    return DB_FILE_LOCK_OK;
#endif
}

void db_file_lock_release(void** handle) {
    if (handle == NULL || *handle == NULL) {
        return;
    }
#if _WIN32
    CloseHandle((HANDLE)*handle);
#else
    int fd = *((int*)*handle);
    flock(fd, LOCK_UN);
    close(fd);
    free(*handle);
#endif
    *handle = NULL;
}