//
// C/C++ compatibility for atomic types.
//
// C11 uses _Atomic(T) and #include <stdatomic.h>.
// C++ uses std::atomic<T> and #include <atomic>.
// This header provides a unified ATOMIC_TYPE(T) macro.
//
// C++ note: <atomic> contains templates that require C++ linkage, so it
// cannot be included inside an extern "C" block. Any C++ translation unit
// that uses this header must ensure <atomic> is included before the first
// extern "C" block. For pure C files, this header includes <stdatomic.h>.
//

#ifndef WAVEDB_ATOMIC_COMPAT_H
#define WAVEDB_ATOMIC_COMPAT_H

#ifdef __cplusplus
  #include <atomic>
  #define ATOMIC_TYPE(T) ::std::atomic<T>
#else
  #include <stdatomic.h>
  #define ATOMIC_TYPE(T) _Atomic(T)
#endif

// ATOMIC_VAR_INIT was removed in C23 (deprecated in C17).
// MSVC's <stdatomic.h> does not provide it. Use plain initialization instead.
#ifndef ATOMIC_VAR_INIT
  #define ATOMIC_VAR_INIT_COMPAT(v) (v)
#else
  #define ATOMIC_VAR_INIT_COMPAT(v) ATOMIC_VAR_INIT(v)
#endif

#endif // WAVEDB_ATOMIC_COMPAT_H