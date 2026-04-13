# WaveDB Technical Debt Audit

Audited 2026-04-13. Tracks all stubbed, missing, broken, or deferred functionality.

---

## Core Engine

### CRITICAL

- **`within_bounds()` is a stub** (`database_iterator.c:57`)
  Always returns 1. Range scans never filter by start/end path bounds. Every scan returns all keys regardless of the requested range.

- **`hbtrie_cursor_next()` is a stub** (`hbtrie.c:522`)
  Labeled "simple placeholder", only increments `chunk_pos`. Full traversal logic missing. Iteration-based APIs cannot work correctly.

- **Dart iterator returns empty data** (`iterator.dart:140`)
  `PathConverter.fromNative` and `IdentifierConverter.fromNative` return empty data because the C API lacks `identifier_get_data` and `path_get_identifiers`. The iterator is non-functional in Dart.

### HIGH

- **WAL file rotation not implemented** (`wal_manager.c:892`)
  When WAL hits max size, writes fail with `-1`. No rotation or compaction. Long-running writes will eventually be rejected.

- **Legacy WAL force option returns ENOTSUP** (`wal_manager.c:726`)
  The `force_legacy` option is explicitly not implemented.

- **HBTrie subtree pruning missing** (`test_hbtrie.cpp` lines 385–815)
  6 disabled `DISABLED_` tests for deletion cleanup. Deleting keys does not reclaim empty parent nodes or compact the trie.

- **Section defragmentation deferred** (`section.c:478`)
  TODO comment: deallocation inserts fragments without merging adjacent ones. No disk-space reclamation — non-contiguous live data is not compacted, so freed space between records is lost until the section is defragmented.

### LOW

- **Win32 lock/condition destroy stubs** (`threadding.c:51,63`)
  `platform_rw_lock_destroy` and `platform_condition_destroy` are empty on Win32. SRWLOCKs and condition variables are kernel objects on Windows that don't need explicit destroy, so these are likely correct no-ops, but should be verified.

---

## GraphQL Layer

### MISSING FEATURES

- **`input` types** — Enum `GRAPHQL_TYPE_INPUT` exists. No parsing, storage, or resolution. Required for mutation arguments with complex structure.

- **`interface` types** — Enum `GRAPHQL_TYPE_INTERFACE` exists. No parsing, no implementation checking, no abstract type resolution.

- **`union` types** — Enum `GRAPHQL_TYPE_UNION` exists. No parsing, no member storage, no resolution to concrete types.

- **`subscription` operations** — Out of scope, not planned.

- **`$variable` support** — No variable definitions in operations, no variable resolution in plan or execution phases. All arguments must be literal values.

- **`PLAN_FILTER`** — Enum value defined, never produced by compiler, never handled in execution. Dead code.

- **Schema validation** — No circular reference checks, interface implementation checks, or union membership checks. Invalid schemas are accepted silently.

- **Batch filtering, sorting, pagination** — `PLAN_BATCH_GET` only processes `id` arguments. No `where`, `orderBy`, `limit`, `offset`, or other filter arguments.

- **`description` strings** — No triple-quoted `"""..."""` description support in SDL parsing.

### PARTIAL / BUGGY

- **Introspection detection is fragile** (`graphql_resolve.c`)
  Uses `strstr(query, "__schema")` and `strstr(query, "__type(")`. Matches inside string literals, comments, and identifiers. Should parse the query first, then check for introspection fields.

- **`@skip`/`@include` only works with string literals** (`graphql_plan.c`)
  `should_skip_field()` compares directive values against string `"true"`/`"false"`. Boolean literal `true` (without quotes) does not work because the parser stores directive arg values as raw string tokens.

- **Object/list default values don't round-trip** (`graphql_schema.c`)
  `parse_default_value()` for `GRAPHQL_LITERAL_OBJECT` and `GRAPHQL_LITERAL_LIST` stores the raw string representation. On reload, these become strings, not structured values.

- **Field argument types discarded** (`graphql_parser.c`)
  `parse_field_definition()` destroys argument type refs. The comment says "Arguments not fully supported in v1". This means `user(id: ID!): User` loses the `ID!` type info for `id`.

- **Mutation sub-selections replace instead of merge** (`graphql_resolve.c`)
  Create/update mutation results only contain explicitly selected fields. Fields written to the DB but not requested are absent from the response.

- **Type loading order risk** (`graphql_schema.c`)
  Types loaded in scan order. If a custom scalar is referenced by a field before it's loaded, it gets misclassified as OBJECT. Could cause incorrect query resolution.

---

## Bindings

- **Dart config overrides ignored** (`wavedb_bindings.dart:876`)
  No native config setters. `chunkSize`, `workerThreads`, WAL settings all use defaults regardless of what the user passes.

- **Dart `createReadStream` conditionally unsupported** (`iterator.dart:94`)
  Throws `NOT_SUPPORTED` if the native symbol is not found.

---

## Tests & Benchmarks

- **6 disabled HBTrie pruning tests** (`test_hbtrie.cpp`)
  `DISABLED_SubtreeDeletion`, `DISABLED_DeepPathPruning`, `DISABLED_PruningWithBranching`, `DISABLED_SequentialDeletionStress`, `DISABLED_PruningWithPartialOverlap`, `DISABLED_PruningPreservesSiblings`

- **Section benchmarks are placeholders** (`benchmark_sections.cpp`)
  Spins `volatile int x = 0` loop. Not measuring real operations.