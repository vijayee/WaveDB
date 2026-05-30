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

/* Compatibility with liboffs actor code (uses method-style atomics) */
#ifdef __cplusplus
  #define ATOMIC(T) std::atomic<T>
  #define ATOMIC_STORE(ptr, val) (ptr)->store(val)
  #define ATOMIC_LOAD(ptr) (ptr)->load()
  #define ATOMIC_EXCHANGE(ptr, val) (ptr)->exchange(val)
  #define ATOMIC_FETCH_ADD(ptr, val) (ptr)->fetch_add(val)
  #define ATOMIC_FETCH_OR(ptr, val) (ptr)->fetch_or(val)
  #define ATOMIC_FETCH_AND(ptr, val) (ptr)->fetch_and(val)
  #define ATOMIC_CAS_STRONG(ptr, expected, desired) (ptr)->compare_exchange_strong(expected, desired)
#else
  #define ATOMIC(T) _Atomic(T)
  #define ATOMIC_STORE(ptr, val) atomic_store(ptr, val)
  #define ATOMIC_LOAD(ptr) atomic_load(ptr)
  #define ATOMIC_EXCHANGE(ptr, val) atomic_exchange(ptr, val)
  #define ATOMIC_FETCH_ADD(ptr, val) atomic_fetch_add(ptr, val)
  #define ATOMIC_FETCH_OR(ptr, val) atomic_fetch_or(ptr, val)
  #define ATOMIC_FETCH_AND(ptr, val) atomic_fetch_and(ptr, val)
  #define ATOMIC_CAS_STRONG(ptr, expected, desired) atomic_compare_exchange_strong(ptr, expected, desired)
#endif

#endif // WAVEDB_ATOMIC_COMPAT_H