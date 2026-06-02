# Sync-Only Mode — Phase 2: Bindings + KVBench Adapter

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose the sync-only flag in Node.js and Dart bindings, and update the KVBench WaveDB adapters to use it for maximum benchmark throughput.

**Architecture:** Each binding exposes a `syncOnly` boolean option that maps to the `database_config_t.sync_only` field. When `syncOnly: true`, the bindings also set `workerThreads: 0` (no pool) and skip WAL config customization (sync-only uses the config's wal_settings). The KVBench sync adapter sets `sync_only=1` for maximal performance. The KVBench async adapter stays in concurrent mode.

**Tech Stack:** Node.js (N-API), Dart FFI, C++, WaveDB C API

---

## File Structure

| File | Role |
|------|------|
| `bindings/nodejs/src/database.cc` | Parse `syncOnly` from JS options, pass to config |
| `bindings/dart/lib/src/database.dart` | Add `syncOnly` to `WaveDBConfig` class |
| `bindings/dart/lib/src/native/wavedb_bindings.dart` | Wire `syncOnly` through `databaseCreateWithConfig` and `databaseCreateEncrypted` |
| `KVBench/adapters/wavedb/bench_wavedb.cpp` | Set `sync_only=1` for sync adapter |
| `KVBench/adapters/wavedb/bench_wavedb_async.cpp` | Keep concurrent mode (no change needed) |

---

### Task 7: Update Node.js binding

**Files:**
- Modify: `bindings/nodejs/src/database.cc:56-277`

**Context:** The Node.js constructor already parses an `options` object with `wal`, `encryption`, etc. We add `syncOnly` to this parsing. When `syncOnly: true`, `workerThreads` defaults to 0.

- [ ] **Step 1: Parse syncOnly from JS options**

In the constructor (around line 90-153, where other options are parsed), add parsing for `syncOnly`:

```cpp
    // Parse syncOnly option
    bool sync_only = false;
    if (options.Has("syncOnly")) {
        sync_only = options.Get("syncOnly").As<Napi::Boolean>().Value();
    }
```

- [ ] **Step 2: Apply sync_only to config**

After all options are parsed and before calling `database_create_with_config` or `database_create_encrypted`, add:

```cpp
    if (sync_only) {
        database_config_set_sync_only(config, 1);
        // Sync-only mode doesn't use worker threads
        database_config_set_worker_threads(config, 0);
    }
```

- [ ] **Step 3: Handle sync_only in encrypted path**

The encrypted path at lines 161-245 also uses `database_create_encrypted`. The `encrypted_database_config_t` contains a base `database_config_t` — apply `sync_only` to it before calling `database_create_encrypted`:

```cpp
    if (sync_only) {
        database_config_set_sync_only(enc_config->base_config, 1);
        database_config_set_worker_threads(enc_config->base_config, 0);
    }
```

(Check if `encrypted_database_config_t` provides access to the base config. If not, set `sync_only` before constructing the encrypted config.)

- [ ] **Step 4: Build Node.js binding**

```bash
cd bindings/nodejs && npm run build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 5: Write quick smoke test**

Create `bindings/nodejs/test_sync_only.js`:

```js
const WaveDB = require('./build/Release/wavedb.node');
const fs = require('fs');

const dbPath = '/tmp/test_node_sync_only';
try { fs.rmSync(dbPath, { recursive: true }); } catch(e) {}

const db = new WaveDB(dbPath, { syncOnly: true, enablePersist: true });

db.put('hello', 'world');
const result = db.get('hello');
console.log('syncOnly get:', result);  // Expected: "world"

db.close();
fs.rmSync(dbPath, { recursive: true });
console.log('Node.js sync_only smoke test: PASS');
```

Run: `node bindings/nodejs/test_sync_only.js`

Expected: `syncOnly get: world`, `Node.js sync_only smoke test: PASS`

- [ ] **Step 6: Commit**

```bash
git add bindings/nodejs/src/database.cc bindings/nodejs/test_sync_only.js
git commit -m "feat(nodejs): expose syncOnly option for sync-only mode"
```

---

### Task 8: Update Dart binding

**Files:**
- Modify: `bindings/dart/lib/src/database.dart:16-63`
- Modify: `bindings/dart/lib/src/native/wavedb_bindings.dart:1204-1365`

**Context:** The Dart `WaveDBConfig` class mirrors `database_config_t`. The FFI binding in `wavedb_bindings.dart` calls C config setter functions to apply overrides.

- [ ] **Step 1: Add `syncOnly` to `WaveDBConfig`**

In `WaveDBConfig` class (line ~16), add:

```dart
  /// Whether to run in sync-only mode (no concurrency control).
  /// When true, pool and wheel are not created.
  final bool syncOnly;
  
  WaveDBConfig({
    ...
    this.syncOnly = false,
  });
```

- [ ] **Step 2: Add C function pointer for `database_config_set_sync_only`**

In `wavedb_bindings.dart`, add the typedef and lazy-loaded function pointer following the pattern of other config setters (lines 900-934):

```dart
// Typedef
typedef DatabaseConfigSetSyncOnlyC = Void Function(Pointer<database_config_t>, Int8);
typedef DatabaseConfigSetSyncOnly = void Function(Pointer<database_config_t>, int);

// Lazy function pointer
final DatabaseConfigSetSyncOnly _databaseConfigSetSyncOnly = ...

// Lookup: 'database_config_set_sync_only'
```

- [ ] **Step 3: Wire syncOnly in `databaseCreateWithConfig()`**

In the `databaseCreateWithConfig()` function (line 1204), after the other config setters, add:

```dart
    if (config.syncOnly) {
      _databaseConfigSetSyncOnly(cConfig, 1);
    }
```

- [ ] **Step 4: Wire syncOnly in `databaseCreateEncrypted()`**

Same as Step 3, in the `databaseCreateEncrypted()` function (line 1278).

- [ ] **Step 5: Build and test Dart binding**

```bash
cd bindings/dart && dart run test --run-skipped 2>&1 | tail -20
```

Expected: All existing tests pass.

- [ ] **Step 6: Commit**

```bash
git add bindings/dart/lib/src/database.dart bindings/dart/lib/src/native/wavedb_bindings.dart
git commit -m "feat(dart): expose syncOnly config option for sync-only mode"
```

---

### Task 9: Update KVBench adapter to use sync_only

**Files:**
- Modify: `KVBench/adapters/wavedb/bench_wavedb.cpp:30-64`

**Context:** The sync adapter currently calls `database_create()` which defaults to concurrent mode (creates pool, tx_manager, write_locks). Switching to `sync_only=1` via `database_create_with_config` skips all that overhead while keeping WAL durability.

- [ ] **Step 1: Rewrite `WaveDBSyncAdapter::Open()` to use `database_create_with_config`**

Replace the current `Open()` method's `database_create()` call with a config-based approach:

```cpp
    bool Open(const char* path, size_t /* num_threads_hint */) {
        mkdir(path, 0755);

        database_config_t* config = database_config_default();
        if (!config) return false;

        // Apply sync-only configuration
        database_config_set_sync_only(config, 1);
        database_config_set_lru_memory_mb(config, 50);
        database_config_set_enable_persist(config, 1);
        database_config_set_wal_sync_mode(config, WAL_SYNC_ASYNC);
        database_config_set_wal_debounce_ms(config, 250);
        database_config_set_wal_idle_threshold_ms(config, 10000);
        database_config_set_wal_compact_interval_ms(config, 60000);
        database_config_set_wal_max_file_size(config, 100 * 1024 * 1024);

        int error_code = 0;
        db_ = database_create_with_config(path, config, &error_code);
        database_config_destroy(config);

        if (!db_ || error_code != 0) {
            std::cerr << "Failed to create database, error_code=" << error_code << std::endl;
            return false;
        }

        std::cout << "WaveDB (sync-only) opened at " << path
                  << " (WAL_ASYNC, 50MB LRU, no concurrency control)" << std::endl;
        return true;
    }
```

- [ ] **Step 2: Ensure `database_config.h` is included**

Check that the includes at the top have `"Database/database_config.h"`. If not, add:

```cpp
extern "C" {
#include "Database/database.h"
#include "Database/database_config.h"
...
}
```

- [ ] **Step 3: Add config setter function declarations**

If the config setter functions are not yet declared in `database_config.h`, they need to be added. Check `src/Database/database_config.h` for:
- `database_config_set_lru_memory_mb`
- `database_config_set_enable_persist`
- `database_config_set_wal_sync_mode`
- etc.

If these don't exist as individual setters, use direct struct field access instead (since the adapter is C++ and has access to the struct definition).

- [ ] **Step 4: Build KVBench**

```bash
cd KVBench && mkdir -p build && cd build && cmake .. && make -j$(nproc) 2>&1 | grep -i error
```

Expected: No errors.

- [ ] **Step 5: Run quick smoke benchmark**

```bash
cd KVBench && ./build/adapters/wavedb/bench_wavedb \
    --workload workloads/w1_combined.txt \
    --db-path /tmp/kvbench_sync_only \
    --warmup 1000
```

Expected: Throughput should be higher than the previous ~290K ops/sec (baseline without sync_only).

- [ ] **Step 6: Commit**

```bash
git add KVBench/adapters/wavedb/bench_wavedb.cpp
git commit -m "perf(kvbench): use sync_only mode for WaveDB sync adapter (skip concurrency control)"
```

---

## Verification Checklist

After both phases are complete:

1. Run C test suite:
   ```bash
   cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure
   ```
   - [ ] All existing tests still pass
   - [ ] `test_sync_only` passes

2. Run Node.js binding tests:
   ```bash
   cd bindings/nodejs && npm test
   ```
   - [ ] All tests pass

3. Run Dart binding tests:
   ```bash
   cd bindings/dart && dart test
   ```
   - [ ] All tests pass

4. Run KVBench benchmarks:
   ```bash
   cd KVBench && ./run_benchmarks.sh
   ```
   - [ ] WaveDB sync throughput improves measurably
   - [ ] No errors from any adapter

5. Cross-mode compatibility test:
   - Create DB in sync-only mode, write data, close
   - Reopen in async mode, read data, write data, close
   - Reopen in sync-only mode, verify all data readable
   - [ ] Works correctly in all transitions
