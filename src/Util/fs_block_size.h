//
// Created by victor on 3/11/26.
//

#ifndef WAVEDB_FS_BLOCK_SIZE_H
#define WAVEDB_FS_BLOCK_SIZE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the filesystem block size for the given path.
 *
 * @param path Path to a file or directory on the filesystem.
 *             If NULL, uses the current working directory.
 * @return The optimal block size for I/O operations in bytes.
 *         Returns 0 on error.
 */
uint64_t fs_block_size_get(const char* path);

#ifdef __cplusplus
}
#endif

#endif //WAVEDB_FS_BLOCK_SIZE_H