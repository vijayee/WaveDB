//
// Windows header ordering compatibility layer
//
// On Windows, <winsock2.h> MUST be included before <windows.h> to avoid
// redefinition errors. This header ensures that ordering is correct
// project-wide. Include this header BEFORE any other Windows headers.
//
// For source files that need Windows APIs, include this header first.
// For headers that need Windows types (CRITICAL_SECTION, etc.), include this
// header instead of <windows.h> directly.
//
#ifndef WAVEDB_WINDOWS_COMPAT_H
#define WAVEDB_WINDOWS_COMPAT_H

#if _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#endif

#endif /* WAVEDB_WINDOWS_COMPAT_H */