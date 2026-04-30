# WaveDB Benchmark Instructions — Windows (Ryzen 9700X)

## Background

We ran benchmarks on a laptop with an Intel i7-1260P and discovered 30-50% run-to-run variance caused by the hybrid P-core/E-core architecture and `powersave` CPU governor. The i7-1260P has 4 P-cores (up to 4.7 GHz with HT) and 8 E-cores (up to 3.4 GHz). The scheduler unpredictably migrates benchmark threads between core types, and the `powersave` governor allows frequency to swing between 400 MHz and 4.7 GHz during measurement.

Key findings from the laptop benchmarks (all in ops/sec, sync API, single-threaded):

| Operation | Best run | Worst run | Variance |
|-----------|---------:|----------:|---------:|
| Put       |  410,462 |   215,177 |    ~48%  |
| Get       |2,107,971 | 1,058,075 |    ~50%  |
| Mixed     |2,338,845 | 1,128,591 |    ~52%  |
| Delete    |  257,972 |   153,861 |    ~40%  |

**Conclusion: No code regression was found.** The variance between commits was entirely within the noise band of the hardware. The README numbers (Put 446K, Get 2.11M, etc.) were best-case single runs, not averages.

The Ryzen 9700X is a single-CCD, 8-core Zen 5 desktop chip. It should produce stable numbers with <5% variance. The goal is to establish reliable baseline benchmarks on this machine.

---

## Prerequisites

### Build Tools

Install one of these toolchains:

**Option A: Visual Studio 2022 + CMake (recommended)**
1. Install Visual Studio 2022 with the "Desktop development with C++" workload
2. Install CMake (via VS installer or standalone)
3. Ensure `cl.exe`, `cmake`, and `ninja` are on PATH (open "Developer Command Prompt for VS 2022")

**Option B: MSYS2 + MinGW**
1. Install MSYS2 from https://www.msys2.org/
2. In MSYS2 UCRT64 shell: `pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja`

### OpenSSL (for encryption support)

Encryption benchmarks require OpenSSL. If you only care about non-encrypted benchmarks, you can skip this, but the build will need minor CMakeLists.txt changes (see "Known Build Issues" below).

For full build:
- **MSVC**: Install via vcpkg (`vcpkg install openssl`) or use a prebuilt binary from https://slproweb.com/products/Win32OpenSSL.html
- **MinGW**: `pacman -S mingw-w64-ucrt-x86_64-openssl`

---

## Known Build Issues on Windows

The project has a partial Windows compatibility layer but several files need fixes before it compiles. The agent **must** fix these before running benchmarks.

### 1. CMakeLists.txt — Compiler flags (line 10)

**Problem:** GCC/Clang flags `-Wall -Wextra -Werror=return-type` are invalid under MSVC.

**Fix:** Wrap in a compiler check:
```cmake
if(MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W4")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror=return-type")
endif()
```

Also line ~148: `target_link_libraries(wavedb PUBLIC atomic)` — MSVC links atomics implicitly, this line should be in an `else()` block or guarded with `if(NOT MSVC)`.

### 2. CMakeLists.txt — Threads::Threads on WIN32 (line 151-158)

**Problem:** `find_package(Threads)` is not called in the `WIN32` branch, but many benchmark and test targets unconditionally link `Threads::Threads`.

**Fix:** Move `find_package(Threads REQUIRED)` before the `if(WIN32)` block so it's always available, or add it inside the WIN32 branch too. On Windows, CMake's `Threads::Threads` resolves to no-op (Windows threads are in kernel32).

### 3. CMakeLists.txt — OpenSSL on WIN32 (line 151-158)

**Problem:** OpenSSL is not linked in the WIN32 branch.

**Fix:** If OpenSSL is available via vcpkg, add `find_package(OpenSSL)` with a fallback:
```cmake
if(WIN32)
    target_compile_definitions(wavedb PRIVATE _WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(wavedb PUBLIC Threads::Threads)
    find_package(OpenSSL)
    if(OpenSSL_FOUND)
        target_link_libraries(wavedb PUBLIC OpenSSL::Crypto)
        target_include_directories(wavedb PRIVATE ${OPENSSL_INCLUDE_DIR})
    endif()
else()
    find_package(Threads REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(wavedb PUBLIC Threads::Threads OpenSSL::Crypto)
    target_include_directories(wavedb PRIVATE ${OPENSSL_INCLUDE_DIR})
endif()
```

### 4. page_file.c — POSIX I/O (no `_WIN32` guards)

**Problem:** Uses `open()`, `read()`, `write()`, `close()`, `posix_fadvise`, `S_IRUSR`/`S_IWUSR` permissions. None of these exist in MSVC.

**Fix:** Add `#if _WIN32` blocks using `_open()`, `_read()`, `_write()`, `_close()` with `#include <io.h>` and `#include <fcntl.h>`. Replace `posix_fadvise` with no-op on Windows (it's an optimization hint, not correctness). Replace permission mode bits with `_S_IREAD | _S_IWRITE`.

### 5. mkdir_p.c — POSIX mkdir (no `_WIN32` guard)

**Problem:** Uses `mkdir(path, S_IRWXU)`, `<libgen.h>`, `dirname()`, `S_ISDIR()`, `EEXIST`.

**Fix:** Add `#if _WIN32` block:
```c
#if _WIN32
#include <direct.h>
#include <sys/stat.h>
#define mkdir(path, mode) _mkdir(path)
// dirname() replacement: extract directory portion of path
// S_ISDIR macro: use _S_IFDIR from sys/stat.h
#else
#include <libgen.h>
#include <sys/stat.h>
#endif
```

Note: Windows `_mkdir()` takes only one argument (no mode). The `EEXIST` constant exists in MSVC's `<errno.h>`.

### 6. wal.c — fsync (no `_WIN32` guard)

**Problem:** Uses `fsync(fd)` which does not exist on Windows.

**Fix:** Add a platform wrapper:
```c
#if _WIN32
#include <io.h>
#define fsync(fd) _commit(fd)
#else
#include <unistd.h>
#endif
```

### 7. wal_compactor.c — Raw pthread calls (no platform abstraction)

**Problem:** Uses `pthread_create`/`pthread_join` directly instead of the project's `platform_*` abstractions.

**Fix:** Replace with `platform_thread_create`/`platform_join` or add `#if _WIN32` blocks using `CreateThread`/`WaitForSingleObject`.

### 8. transaction_id.c — clock_gettime (line ~44)

**Problem:** Uses `clock_gettime(CLOCK_MONOTONIC, ...)` unconditionally. MSVC does not have `clock_gettime`.

**Fix:** Add `#if _WIN32` block using `QueryPerformanceCounter` or `GetTickCount64`.

### 9. Benchmark code — POSIX-specific includes

**Problem in benchmark_database_sync.cpp:**
- `#include <unistd.h>` for `getpid()`
- `#include <sys/stat.h>` for `mkdir()`
- Uses `/tmp/` for temp directories
- Uses `rm -rf` via `system()` for cleanup
- Uses `mkdir -p` via `system()` for output directory

**Fix:** Add Windows equivalents:
```cpp
#if _WIN32
#include <windows.h>  // for GetCurrentProcessId()
#define getpid() GetCurrentProcessId()
#include <direct.h>   // for _mkdir()
#define mkdir(path, mode) _mkdir(path)
// Use TEMP env var instead of /tmp
// Use "rmdir /s /q" or SHFileOperation for cleanup
#endif
```

### 10. Spinlock atomics — GCC builtins

**Problem:** `threadding.h`/`.c` uses `__atomic_exchange_n` and `__atomic_store_n` which are GCC builtins. MSVC uses `_InterlockedExchange8` etc.

**Fix:** The existing `_WIN32` branch in `threadding.h` should already handle this. Verify that the spinlock implementation uses the platform macros. If raw `__atomic_*` builtins are used outside the `#if _WIN32` guard, add MSVC equivalents.

---

## Build Steps

### MSVC (Visual Studio 2022)

Open "Developer Command Prompt for VS 2022" or "Developer PowerShell":

```bat
cd <repo-root>
mkdir build && cd build
cmake .. -G Ninja -DBUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
ninja benchmark_database benchmark_database_sync benchmark_database_sync_only
```

### MinGW (MSYS2 UCRT64)

```bash
cd <repo-root>
mkdir build && cd build
cmake .. -G Ninja -DBUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
ninja benchmark_database benchmark_database_sync benchmark_database_sync_only
```

If the build fails, fix the issues listed above one at a time. The most critical fixes are items 1-6 (CMakeLists + page_file + mkdir_p + wal + transaction_id). Items 7-10 may not block the sync-only benchmark which doesn't use WAL compaction.

---

## Running Benchmarks

### Step 1: Verify CPU governor

On desktop Windows, CPU governor is typically `performance` by default. Verify by checking CPU frequency is near max:

```powershell
# In PowerShell - check max frequency
Get-CimInstance Win32_Processor | Select-Object Name, MaxClockSpeed, CurrentClockSpeed
```

If `CurrentClockSpeed` is significantly below `MaxClockSpeed`, set power plan:
```powershell
powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c  # High Performance
```

### Step 2: Run each benchmark 5 times

The benchmark executables are in the `build/` directory after building. Run each one 5 times to establish stability.

**Sync benchmark (most important):**
```bat
cd <repo-root>\build
for /L %i in (1,1,5) do benchmark_database_sync.exe
```

Or in PowerShell:
```powershell
1..5 | ForEach-Object { .\benchmark_database_sync.exe }
```

**Sync-only benchmark:**
```powershell
1..5 | ForEach-Object { .\benchmark_database_sync_only.exe }
```

**Async benchmark:**
```powershell
1..5 | ForEach-Object { .\benchmark_database.exe }
```

### Step 3: Collect and report results

For each benchmark, extract the `Throughput:` lines from each run. Calculate the average and coefficient of variation (CV = stddev/mean). On the Ryzen 9700X, we expect CV < 5% for all operations.

Report the results in this format:

```
## Sync Throughput (ops/sec) — 5-run average ± CV

| Operation | Run 1    | Run 2    | Run 3    | Run 4    | Run 5    | Avg      | CV   |
|-----------|---------:|---------:|---------:|---------:|---------:|---------:|-----:|
| Put       |          |          |          |          |          |          |      |
| Get       |          |          |          |          |          |          |      |
| Mixed     |          |          |          |          |          |          |      |
| Delete    |          |          |          |          |          |          |      |
```

### Step 4: Compare against laptop numbers

Once you have stable 5-run averages, compare against the laptop best-case numbers:

| Operation | Laptop best | Laptop avg (5 runs) | Desktop avg (5 runs) | Improvement |
|-----------|-----------:|--------------------:|---------------------:|------------:|
| Put       |    410,462 |             ~319,000 |                      |             |
| Get       |  2,107,971 |           ~1,740,000 |                      |             |
| Mixed     |  2,338,845 |           ~1,990,000 |                      |             |
| Delete    |    257,972 |             ~180,000 |                      |             |

---

## Expected Results on Ryzen 9700X

The 9700X has 8 Zen 5 cores at 5.5 GHz boost / 3.8 GHz base, no hybrid architecture. We expect:

1. **Variance < 5%** between runs (vs 40-50% on the laptop)
2. **Throughput 20-50% higher** than the laptop best-case, due to higher sustained clock and no E-core scheduling
3. **Put and Delete** should benefit most from the higher sustained clock (they do more work per operation)
4. **Get and Mixed** (which are LRU-cache hot-path dominated) may show smaller gains since they're already very fast

---

## If Variance Is Still High

If you see >5% variance on the desktop:

1. Close all other applications (browsers, IDEs, etc.)
2. Disable Windows Defender real-time scanning for the benchmark temp directory
3. Set process affinity: `Start-Process -FilePath .\benchmark_database_sync.exe -Affinity 0xFF` (pins to all 8 cores)
4. Set process priority: `Start-Process -FilePath .\benchmark_database_sync.exe -Priority High`
5. Run 5+ times and discard the first run (cold-start outlier)