//
// C/C++ compatibility for atomic types.
//
// C11 uses _Atomic(T) and #include <stdatomic.h>.
// C++ uses std::atomic<T> and #include <atomic>.
// This header provides a unified ATOMIC_TYPE(T) macro.
//
// MSVC C mode: <stdatomic.h> requires /std:c11, but node-gyp adds /std:c++20
// which conflicts with /std:c11 in AdditionalOptions. We provide Interlocked-
// based fallbacks for MSVC C mode so /std:c11 is not needed.
//

#ifndef WAVEDB_ATOMIC_COMPAT_H
#define WAVEDB_ATOMIC_COMPAT_H

#ifdef __cplusplus
  // C++ mode: use std::atomic (works with /std:c++20)
  #include <atomic>
  #define ATOMIC_TYPE(T) ::std::atomic<T>
  #define ATOMIC_VAR_INIT_COMPAT(v) (v)

#elif defined(_MSC_VER)
  // MSVC C mode: /std:c11 conflicts with /std:c++20 from node-gyp,
  // so <stdatomic.h> is unavailable. Use Interlocked functions instead.
  #include <windows.h>

  // ATOMIC_TYPE(T) just declares a raw value — atomicity comes from the
  // Interlocked-based macros below which cast to volatile LONG/LONG64.
  #define ATOMIC_TYPE(T) T

  // Memory order stubs — Interlocked functions provide full barriers
  typedef enum {
    memory_order_relaxed = 0,
    memory_order_consume = 1,
    memory_order_acquire = 2,
    memory_order_release = 3,
    memory_order_acq_rel = 4,
    memory_order_seq_cst = 5
  } memory_order;

  /* --- Internal helpers: cast pointer to volatile LONG* or LONG64* --- */

  static inline volatile LONG* _wdb_atomic_as_long(void* p) {
    return (volatile LONG*)p;
  }
  static inline volatile LONG64* _wdb_atomic_as_long64(void* p) {
    return (volatile LONG64*)p;
  }

  /* --- Size-based dispatch: 8-byte types use LONG64, else LONG --- */

  #define _WDB_8BYTE(type_or_expr) (sizeof(type_or_expr) == 8)

  /* --- atomic_load --- */
  #define atomic_load(obj) \
    (_WDB_8BYTE(*(obj)) \
      ? (InterlockedCompareExchange64(_wdb_atomic_as_long64((void*)(obj)), 0, 0)) \
      : ((LONG)InterlockedCompareExchange(_wdb_atomic_as_long((void*)(obj)), 0, 0)))

  /* --- atomic_store --- */
  #define atomic_store(obj, val) \
    (_WDB_8BYTE(*(obj)) \
      ? (InterlockedExchange64(_wdb_atomic_as_long64((void*)(obj)), (LONG64)(val))) \
      : (InterlockedExchange(_wdb_atomic_as_long((void*)(obj)), (LONG)(val)))

  /* --- atomic_store_explicit --- */
  #define atomic_store_explicit(obj, val, order) atomic_store(obj, val)

  /* --- atomic_fetch_add --- */
  #define atomic_fetch_add(obj, val) \
    (_WDB_8BYTE(*(obj)) \
      ? (InterlockedExchangeAdd64(_wdb_atomic_as_long64((void*)(obj)), (LONG64)(val))) \
      : (InterlockedExchangeAdd(_wdb_atomic_as_long((void*)(obj)), (LONG)(val)))

  /* --- atomic_fetch_sub --- */
  #define atomic_fetch_sub(obj, val) \
    (_WDB_8BYTE(*(obj)) \
      ? (InterlockedExchangeAdd64(_wdb_atomic_as_long64((void*)(obj)), -(LONG64)(val))) \
      : (InterlockedExchangeAdd(_wdb_atomic_as_long((void*)(obj)), -(LONG)(val)))

  /* --- atomic_exchange_explicit --- */
  #define atomic_exchange_explicit(obj, val, order) atomic_store(obj, val)

  /* --- atomic_compare_exchange_weak --- */
  // Returns 1 on success, 0 on failure. Updates *expected on failure.
  static inline int _wdb_cas32(volatile LONG* obj, LONG* expected, LONG desired) {
    LONG old = InterlockedCompareExchange(obj, desired, *expected);
    if (old == *expected) return 1;
    *expected = old;
    return 0;
  }
  static inline int _wdb_cas64(volatile LONG64* obj, LONG64* expected, LONG64 desired) {
    LONG64 old = InterlockedCompareExchange64(obj, desired, *expected);
    if (old == *expected) return 1;
    *expected = old;
    return 0;
  }

  #define atomic_compare_exchange_weak(obj, expected, desired) \
    (_WDB_8BYTE(*(obj)) \
      ? _wdb_cas64(_wdb_atomic_as_long64((void*)(obj)), \
                    (LONG64*)(expected), (LONG64)(desired)) \
      : _wdb_cas32(_wdb_atomic_as_long((void*)(obj)), \
                    (LONG*)(expected), (LONG)(desired)))

  /* --- atomic_init --- */
  #define atomic_init(obj, val) (*(obj) = (val))

  /* --- atomic_thread_fence --- */
  static inline void atomic_thread_fence(memory_order order) {
    MemoryBarrier();
    (void)order;
  }

  /* --- ATOMIC_VAR_INIT compat --- */
  #define ATOMIC_VAR_INIT_COMPAT(v) (v)

#else
  // Non-MSVC C mode (GCC/Clang): use C11 stdatomic.h
  #include <stdatomic.h>
  #define ATOMIC_TYPE(T) _Atomic(T)

  #ifndef ATOMIC_VAR_INIT
    #define ATOMIC_VAR_INIT_COMPAT(v) (v)
  #else
    #define ATOMIC_VAR_INIT_COMPAT(v) ATOMIC_VAR_INIT(v)
  #endif
#endif

#endif // WAVEDB_ATOMIC_COMPAT_H