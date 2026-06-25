//
// C/C++ compatibility for atomic types.
//
// C11 uses _Atomic(T) and #include <stdatomic.h>.
// C++ uses std::atomic<T> and #include <atomic>.
// This header provides a unified ATOMIC_TYPE(T) macro.
//
// MSVC C mode: <stdatomic.h> requires /std:c11, but node-gyp adds
// /std:c++20 globally which conflicts. We use Interlocked-based fallbacks
// instead, which work without any special compiler flags.
//
// For types > 8 bytes (e.g. transaction_id_t), a spinlock-based
// fallback provides correctness without Interlocked support.
//

#ifndef WAVEDB_ATOMIC_COMPAT_H
#define WAVEDB_ATOMIC_COMPAT_H

#ifdef __cplusplus
  // C++ mode: use std::atomic (works with /std:c++20)
  #include <atomic>
  #define ATOMIC_TYPE(T) ::std::atomic<T>
  #define ATOMIC_TYPE64  ::std::atomic<uint64_t>
  #define ATOMIC_TYPE_PTR(T) ::std::atomic<T>
  #define ATOMIC_TYPE_TXN ::std::atomic<transaction_id_t>
  typedef ::std::atomic<uint_fast64_t> atomic_uint_fast64_t;
  typedef ::std::atomic<size_t> atomic_size_t;
  #define ATOMIC_VAR_INIT_COMPAT(v) (v)

  #define atomic_load_txn(obj, result) (*(result) = (obj)->load())
  #define atomic_store_txn(obj, val) (obj)->store(val)
  #define atomic_compare_exchange_txn(obj, expected, desired) \
      (obj)->compare_exchange_weak(*(expected), desired)
  #define atomic_init_txn(obj, val) (obj)->store(val)

  #define atomic_load_ptr(obj, T) (obj)->load()
  #define atomic_store_ptr(obj, val) (obj)->store(val)
  #define atomic_compare_exchange_ptr(obj, expected, desired) \
      (obj)->compare_exchange_weak(*(expected), desired)

#elif defined(_MSC_VER)
  // MSVC C mode: <stdatomic.h> requires /std:c11 which conflicts with
  // /std:c++20 that node-gyp adds globally. Use Interlocked functions instead.
  // Include winsock2.h before windows.h to avoid the legacy winsock.h
  // redefinition conflict (see Util/windows_compat.h for the ordering rule).
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <string.h>

  /* ── Wrapper structs for size-appropriate atomic storage ──
   *
   * Interlocked functions only operate on LONG (4-byte) and LONG64 (8-byte),
   * so small types (uint8_t, uint16_t) are promoted to LONG.
   * Types > 8 bytes use a spinlock for correctness.
   *
   * Usage:
   *   ATOMIC_TYPE(int)        → _wdb_atomic4   (≤4-byte atomics)
   *   ATOMIC_TYPE64            → _wdb_atomic8   (8-byte atomics)
   *   ATOMIC_TYPE_TXN          → _wdb_atomic_txn (transaction_id_t atomics)
   */

  typedef struct _wdb_atomic4  { LONG   v; } _wdb_atomic4;
  typedef struct _wdb_atomic8  { LONG64 v; } _wdb_atomic8;
  typedef struct _wdb_atomic_txn {
      char   data[24];  // Stores transaction_id_t as raw bytes (3 × uint64_t)
      LONG   lock;       // spinlock: 0=unlocked, 1=locked
  } _wdb_atomic_txn;

  #define ATOMIC_TYPE(T)    _wdb_atomic4   /* ≤4-byte: int, uint8_t, uint16_t, uint32_t */
  #define ATOMIC_TYPE64     _wdb_atomic8    /* 8-byte: uint64_t */
  #define ATOMIC_TYPE_PTR(T) _wdb_atomic8   /* pointer-sized: hbtrie_node_t*, etc. */
  #define ATOMIC_TYPE_TXN   _wdb_atomic_txn /* 24-byte: transaction_id_t */
  typedef _wdb_atomic8 atomic_uint_fast64_t;
  typedef _wdb_atomic8 atomic_size_t;

  // Memory order stubs — Interlocked functions provide full barriers
  typedef enum {
    memory_order_relaxed = 0,
    memory_order_consume = 1,
    memory_order_acquire = 2,
    memory_order_release = 3,
    memory_order_acq_rel = 4,
    memory_order_seq_cst = 5
  } memory_order;

  /* ── Inline helpers for 4/8-byte atomics ── */

  static inline LONG _wdb_load4(volatile LONG* p) {
    return InterlockedCompareExchange(p, 0, 0);
  }
  static inline LONG64 _wdb_load8(volatile LONG64* p) {
    return InterlockedCompareExchange64(p, 0, 0);
  }
  static inline void _wdb_store4(volatile LONG* p, LONG v) {
    InterlockedExchange(p, v);
  }
  static inline void _wdb_store8(volatile LONG64* p, LONG64 v) {
    InterlockedExchange64(p, v);
  }
  static inline LONG _wdb_xchg4(volatile LONG* p, LONG v) {
    return InterlockedExchange(p, v);
  }
  static inline LONG64 _wdb_xchg8(volatile LONG64* p, LONG64 v) {
    return InterlockedExchange64(p, v);
  }
  static inline LONG _wdb_add4(volatile LONG* p, LONG v) {
    return InterlockedExchangeAdd(p, v);
  }
  static inline LONG64 _wdb_add8(volatile LONG64* p, LONG64 v) {
    return InterlockedExchangeAdd64(p, v);
  }
  static inline int _wdb_cas4(volatile LONG* obj, LONG* expected, LONG desired) {
    LONG old = InterlockedCompareExchange(obj, desired, *expected);
    if (old == *expected) return 1;
    *expected = old;
    return 0;
  }
  static inline int _wdb_cas8(volatile LONG64* obj, LONG64* expected, LONG64 desired) {
    LONG64 old = InterlockedCompareExchange64(obj, desired, *expected);
    if (old == *expected) return 1;
    *expected = old;
    return 0;
  }

  /* ── Inline helpers for transaction_id_t (24-byte) atomics ── */
  /* Uses spinlock since no Interlocked operation handles 24 bytes. */

  static inline void _wdb_txn_lock(volatile LONG* lock) {
    while (InterlockedExchange(lock, 1)) {
      /* Spin */
    }
  }
  static inline void _wdb_txn_unlock(volatile LONG* lock) {
    InterlockedExchange(lock, 0);
  }

  /* ── Public macros — dispatch via sizeof((obj)->v) for 4/8-byte ── */

  #define atomic_load(obj) \
    (sizeof((obj)->v) == 8 \
      ? (LONG64)_wdb_load8(&(obj)->v) \
      : (LONG)_wdb_load4(&(obj)->v))

  #define atomic_store(obj, val) \
    (sizeof((obj)->v) == 8 \
      ? _wdb_store8(&(obj)->v, (LONG64)(val)) \
      : _wdb_store4(&(obj)->v, (LONG)(val)))

  #define atomic_store_explicit(obj, val, order) atomic_store(obj, val)

  #define atomic_fetch_add(obj, val) \
    (sizeof((obj)->v) == 8 \
      ? (LONG64)_wdb_add8(&(obj)->v, (LONG64)(val)) \
      : (LONG)_wdb_add4(&(obj)->v, (LONG)(val)))

  #define atomic_fetch_sub(obj, val) \
    (sizeof((obj)->v) == 8 \
      ? (LONG64)_wdb_add8(&(obj)->v, -(LONG64)(val)) \
      : (LONG)_wdb_add4(&(obj)->v, -(LONG)(val)))

  #define atomic_exchange_explicit(obj, val, order) \
    (sizeof((obj)->v) == 8 \
      ? (LONG64)_wdb_xchg8(&(obj)->v, (LONG64)(val)) \
      : (LONG)_wdb_xchg4(&(obj)->v, (LONG)(val)))

  #define atomic_compare_exchange_weak(obj, expected, desired) \
    (sizeof((obj)->v) == 8 \
      ? _wdb_cas8(&(obj)->v, (LONG64*)(expected), (LONG64)(desired)) \
      : _wdb_cas4(&(obj)->v, (LONG*)(expected), (LONG)(desired)))

  /* ── Pointer-specific macros for ATOMIC_TYPE_PTR fields ──
   * These cast between LONG64 and pointer types to handle the
   * _wdb_atomic8 storage used for pointers in MSVC C mode. */
  #define atomic_load_ptr(obj, T) ((T)(uintptr_t)_wdb_load8(&(obj)->v))
  #define atomic_store_ptr(obj, val) _wdb_store8(&(obj)->v, (LONG64)(uintptr_t)(val))
  #define atomic_compare_exchange_ptr(obj, expected, desired) \
      _wdb_cas8(&(obj)->v, (LONG64*)(expected), (LONG64)(uintptr_t)(desired))

  /* ── Transaction-ID-specific atomic operations ──
   * These operate on _wdb_atomic_txn fields and use a spinlock
   * since 24-byte values have no hardware CAS support. */

  /* transaction_id_t-specific macros. These use memcpy-based helpers that
   * don't require transaction_id_t to be defined at include time. */
  #define atomic_load_txn(obj, result) _wdb_atomic_txn_load_into((obj), (result))
  #define atomic_store_txn(obj, val) _wdb_atomic_txn_store_from((obj), &(val))
  #define atomic_compare_exchange_txn(obj, expected, desired) \
      _wdb_atomic_txn_cas_from((obj), (expected), &(desired))

  /* Transaction-ID atomics use memcpy to avoid requiring transaction_id_t
   * definition at include time. sizeof(transaction_id_t) == 24. */

  static inline void _wdb_atomic_txn_load_into(_wdb_atomic_txn* obj, void* dst) {
    _wdb_txn_lock(&obj->lock);
    memcpy(dst, obj->data, 24);
    _wdb_txn_unlock(&obj->lock);
  }

  static inline void _wdb_atomic_txn_store_from(_wdb_atomic_txn* obj, const void* src) {
    _wdb_txn_lock(&obj->lock);
    memcpy(obj->data, src, 24);
    _wdb_txn_unlock(&obj->lock);
  }

  static inline int _wdb_atomic_txn_cas_from(_wdb_atomic_txn* obj,
                                               void* expected, const void* desired) {
    _wdb_txn_lock(&obj->lock);
    if (memcmp(obj->data, expected, 24) == 0) {
      memcpy(obj->data, desired, 24);
      _wdb_txn_unlock(&obj->lock);
      return 1;
    }
    memcpy(expected, obj->data, 24);
    _wdb_txn_unlock(&obj->lock);
    return 0;
  }

  /* ── atomic_init ── */
  #define atomic_init(obj, val) ((obj)->v = (val))

  #define atomic_init_txn(obj, val) do { \
    memcpy((obj)->data, &(val), 24); \
    (obj)->lock = 0; \
  } while(0)

  /* ── atomic_thread_fence ── */
  static inline void atomic_thread_fence(memory_order order) {
    MemoryBarrier();
    (void)order;
  }

  /* ── ATOMIC_VAR_INIT compat ── */
  #define ATOMIC_VAR_INIT_COMPAT(v) { (v) }

#else
  // C mode with C11 atomics support (GCC/Clang, or MSVC with /std:c11 /experimental:c11atomics)
  #include <stdatomic.h>
  #define ATOMIC_TYPE(T)    _Atomic(T)
  #define ATOMIC_TYPE64     _Atomic(uint64_t)
  #define ATOMIC_TYPE_PTR(T) _Atomic(T)
  #define ATOMIC_TYPE_TXN   _Atomic(transaction_id_t)
  typedef _Atomic(uint_fast64_t) atomic_uint_fast64_t;
  typedef _Atomic(size_t) atomic_size_t;

  #define atomic_load_txn(obj, result) (*(result) = atomic_load(obj))
  #define atomic_store_txn(obj, val) atomic_store(obj, val)
  #define atomic_compare_exchange_txn(obj, expected, desired) \
      atomic_compare_exchange_weak(obj, expected, desired)
  #define atomic_init_txn(obj, val) atomic_init(obj, val)

  #define atomic_load_ptr(obj, T) atomic_load(obj)
  #define atomic_store_ptr(obj, val) atomic_store(obj, val)
  #define atomic_compare_exchange_ptr(obj, expected, desired) \
      atomic_compare_exchange_weak(obj, expected, desired)

  #ifndef ATOMIC_VAR_INIT
    #define ATOMIC_VAR_INIT_COMPAT(v) (v)
  #else
    #define ATOMIC_VAR_INIT_COMPAT(v) ATOMIC_VAR_INIT(v)
  #endif
#endif

#endif // WAVEDB_ATOMIC_COMPAT_H