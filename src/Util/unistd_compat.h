//
// MSVC unistd compatibility layer
//
// Provides POSIX function mappings on MSVC (close, read, write, lseek, etc.)
// and S_ISDIR macro. On non-MSVC platforms, includes native <unistd.h>.
//
// IMPORTANT: On MSVC, this header includes <winsock2.h> before <windows.h>
// to avoid redefinition errors. Include this before any header that pulls
// in <windows.h>.
//
#ifndef WAVEDB_UNISTD_COMPAT_H
#define WAVEDB_UNISTD_COMPAT_H

#ifdef _MSC_VER

/* Ensure winsock2.h is included before windows.h */
#include "Util/windows_compat.h"

#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <process.h>
#include <sys/stat.h>
#include <BaseTsd.h>  /* for SSIZE_T */

/* ssize_t is not defined by MSVC — map from SSIZE_T */
typedef SSIZE_T ssize_t;

/* POSIX file I/O mappings */
#define close     _close
#define read      _read
#define write     _write
#define lseek     _lseek
#define open      _open
#define unlink    _unlink
#define rmdir     _rmdir
#define getpid    _getpid
#define access    _access
#define fdopen    _fdopen

/* access() mode flags */
#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 1
#endif

/* POSIX mkdir: strip mode argument */
#define mkdir(path, mode) _mkdir(path)

/* Access mode flags for _open */
#ifndef O_BINARY
#define O_BINARY  _O_BINARY
#endif
#ifndef O_CREAT
#define O_CREAT   _O_CREAT
#endif
#ifndef O_TRUNC
#define O_TRUNC   _O_TRUNC
#endif
#ifndef O_RDWR
#define O_RDWR    _O_RDWR
#endif
#ifndef O_RDONLY
#define O_RDONLY  _O_RDONLY
#endif
#ifndef O_WRONLY
#define O_WRONLY  _O_WRONLY
#endif
#ifndef O_EXCL
#define O_EXCL    _O_EXCL
#endif
#ifndef S_IRUSR
#define S_IRUSR   _S_IREAD
#endif
#ifndef S_IWUSR
#define S_IWUSR   _S_IWRITE
#endif
#ifndef S_IRGRP
#define S_IRGRP   0
#endif
#ifndef S_IROTH
#define S_IROTH   0
#endif

/* S_ISDIR is not defined by MSVC */
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif

/* fsync mapping */
#define fsync(fd) _commit(fd)

/* ftruncate mapping */
#define ftruncate(fd, length) _chsize(fd, length)

/* mkstemp: create and open a unique temp file — MSVC lacks this */
static inline int mkstemp(char* tmpl) {
    if (_mktemp(tmpl) == NULL) return -1;
    return _open(tmpl, _O_CREAT | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
}

#else
/* Non-MSVC platforms use native <unistd.h> */
#include <unistd.h>

/* S_ISDIR may not be defined on all platforms */
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

#endif /* _MSC_VER */

#endif /* WAVEDB_UNISTD_COMPAT_H */