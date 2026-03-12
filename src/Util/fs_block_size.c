//
// Created by victor on 3/11/26.
//

#include "fs_block_size.h"

#if _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

uint64_t fs_block_size_get(const char* path) {
#if _WIN32
    char abs_path[MAX_PATH];
    DWORD sectors_per_cluster;
    DWORD bytes_per_sector;
    DWORD free_clusters;
    DWORD total_clusters;

    if (path == NULL) {
        /* Use current directory if path is NULL */
        if (GetCurrentDirectoryA(MAX_PATH, abs_path) == 0) {
            return 0;
        }
    } else {
        /* Get absolute path */
        if (GetFullPathNameA(path, MAX_PATH, abs_path, NULL) == 0) {
            return 0;
        }
    }

    /* Extract drive root (e.g., "C:\") */
    char drive_root[4] = {abs_path[0], ':', '\\', 0};

    if (GetDiskFreeSpaceA(drive_root, &sectors_per_cluster,
                          &bytes_per_sector, &free_clusters, &total_clusters)) {
        return (uint64_t)sectors_per_cluster * bytes_per_sector;
    }
    return 0;
#else
    struct statvfs st;

    const char* target_path = (path != NULL) ? path : ".";
    if (statvfs(target_path, &st) != 0) {
        return 0;
    }

    return (uint64_t)st.f_bsize;
#endif
}