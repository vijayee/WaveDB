//
// MSVC dirent compatibility layer
//
// Provides opendir/readdir/closedir on Windows using FindFirstFileW/FindNextFileW.
// On non-MSVC platforms, includes the native <dirent.h>.
//
#ifndef WAVEDB_DIRENT_COMPAT_H
#define WAVEDB_DIRENT_COMPAT_H

#ifdef _MSC_VER

#include "Util/windows_compat.h"
#include <string.h>
#include <stdlib.h>

/* DT_DIR and DT_UNKNOWN for d_type */
#define DT_UNKNOWN  0
#define DT_DIR      1
#define DT_REG      2

struct dirent {
    char d_name[MAX_PATH];
    unsigned char d_type;
};

typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAW find_data;
    struct dirent entry;
    int first; /* 1 if FindNextFileW hasn't been called yet */
    wchar_t pattern[MAX_PATH + 4]; /* directory\* */
} DIR;

#ifdef __cplusplus
extern "C" {
#endif

static inline DIR* opendir(const char* name) {
    DIR* dir = (DIR*)calloc(1, sizeof(DIR));
    if (!dir) return NULL;

    /* Convert name to wide string for pattern */
    int name_len = (int)strlen(name);
    if (name_len >= MAX_PATH) {
        free(dir);
        return NULL;
    }

    /* Build search pattern: name\\\0 */
    MultiByteToWideChar(CP_UTF8, 0, name, -1, dir->pattern, MAX_PATH);
    int wlen = (int)wcslen(dir->pattern);
    if (wlen > 0 && (dir->pattern[wlen - 1] == L'/' || dir->pattern[wlen - 1] == L'\\')) {
        dir->pattern[wlen] = L'*';
        dir->pattern[wlen + 1] = L'\0';
    } else {
        dir->pattern[wlen] = L'\\';
        dir->pattern[wlen + 1] = L'*';
        dir->pattern[wlen + 2] = L'\0';
    }

    dir->handle = FindFirstFileW(dir->pattern, &dir->find_data);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        free(dir);
        return NULL;
    }
    dir->first = 1;
    return dir;
}

static inline struct dirent* readdir(DIR* dir) {
    if (!dir) return NULL;

    if (dir->first) {
        dir->first = 0;
    } else {
        if (!FindNextFileW(dir->handle, &dir->find_data)) {
            return NULL;
        }
    }

    /* Convert wide filename to UTF-8 */
    WideCharToMultiByte(CP_UTF8, 0, dir->find_data.cFileName, -1,
                        dir->entry.d_name, MAX_PATH, NULL, NULL);

    /* Set d_type */
    if (dir->find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        dir->entry.d_type = DT_DIR;
    } else {
        dir->entry.d_type = DT_REG;
    }

    return &dir->entry;
}

static inline int closedir(DIR* dir) {
    if (!dir) return -1;
    FindClose(dir->handle);
    free(dir);
    return 0;
}

#ifdef __cplusplus
}
#endif

#else
/* Non-MSVC platforms use native dirent */
#include <dirent.h>

#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif

#endif /* _MSC_VER */

#endif /* WAVEDB_DIRENT_COMPAT_H */