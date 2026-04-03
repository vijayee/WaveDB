// lib/src/native/types.dart
import 'dart:ffi';

/// Opaque handle to a WaveDB database
/// Maps to database_t in C
base class database_t extends Opaque {}

/// Opaque handle to a path (sequence of identifiers)
/// Maps to path_t in C
base class path_t extends Opaque {}

/// Opaque handle to an identifier (value or key)
/// Maps to identifier_t in C
base class identifier_t extends Opaque {}

/// Opaque handle to a database iterator
/// Maps to database_iterator_t in C
base class database_iterator_t extends Opaque {}

/// Opaque handle to a buffer
/// Maps to buffer_t in C
base class buffer_t extends Opaque {}

/// Reference counter structure (first field of refcounted structs)
/// Maps to refcounter_t in C
/// Note: Only the count field is defined here for reference counting access.
/// The full struct has additional fields (yield, lock) that are platform-specific
/// and not needed for FFI bindings.
base class refcounter_t extends Struct {
  @Uint16()
  external int count;

  @Uint8()
  external int yield;

  // Note: PLATFORMLOCKTYPE(lock) is platform-specific and varies in size.
  // For FFI purposes, refcounter_t structs are typically accessed through
  // opaque handles, so the full layout is not required.
}