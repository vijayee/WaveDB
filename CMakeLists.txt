cmake_minimum_required(VERSION 3.14)
project(WaveDB C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror=return-type")
set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O3")

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/src/Util)
include_directories(${CMAKE_SOURCE_DIR}/src/Buffer)
include_directories(${CMAKE_SOURCE_DIR}/src/RefCounter)
include_directories(${CMAKE_SOURCE_DIR}/src/HBTrie)
include_directories(${CMAKE_SOURCE_DIR}/src/Database)
include_directories(${CMAKE_SOURCE_DIR}/src/Time)
include_directories(${CMAKE_SOURCE_DIR}/src/Workers)
include_directories(${CMAKE_SOURCE_DIR}/src/Storage)
include_directories(${CMAKE_SOURCE_DIR}/deps/xxhash)
include_directories(${CMAKE_SOURCE_DIR}/deps/hashmap/include)
include_directories(${CMAKE_SOURCE_DIR}/deps/libcbor/src)

# xxHash library
add_library(xxhash STATIC
    deps/xxhash/xxhash.c
)
target_include_directories(xxhash PUBLIC ${CMAKE_SOURCE_DIR}/deps/xxhash)

# libcbor library
add_library(cbor STATIC
    deps/libcbor/src/cbor.c
    deps/libcbor/src/allocators.c
    deps/libcbor/src/cbor/arrays.c
    deps/libcbor/src/cbor/bytestrings.c
    deps/libcbor/src/cbor/callbacks.c
    deps/libcbor/src/cbor/common.c
    deps/libcbor/src/cbor/encoding.c
    deps/libcbor/src/cbor/floats_ctrls.c
    deps/libcbor/src/cbor/ints.c
    deps/libcbor/src/cbor/maps.c
    deps/libcbor/src/cbor/strings.c
    deps/libcbor/src/cbor/tags.c
    deps/libcbor/src/cbor/streaming.c
    deps/libcbor/src/cbor/serialization.c
    deps/libcbor/src/cbor/internal/builder_callbacks.c
    deps/libcbor/src/cbor/internal/encoders.c
    deps/libcbor/src/cbor/internal/loaders.c
    deps/libcbor/src/cbor/internal/memory_utils.c
    deps/libcbor/src/cbor/internal/stack.c
    deps/libcbor/src/cbor/internal/unicode.c
)
target_include_directories(cbor PUBLIC ${CMAKE_SOURCE_DIR}/deps/libcbor/src)
target_compile_definitions(cbor PRIVATE CBOR_PRETTY_PRINTER=ON)

# WaveDB library sources
set(WAVEDB_SOURCES
    # Util
    src/Util/allocator.c
    src/Util/log.c
    src/Util/vec.c
    src/Util/hash.c
    src/Util/threadding.c
    src/Util/mkdir_p.c
    src/Util/path_join.c
    src/Util/get_dir.c
    src/Util/perf_counters.c
    src/Util/memory_pool.c
    src/Util/memory_pool.c

    # Buffer
    src/Buffer/buffer.c

    # RefCounter
    src/RefCounter/refcounter.c

    # HBTrie
    src/HBTrie/chunk.c
    src/HBTrie/identifier.c
    src/HBTrie/path.c
    src/HBTrie/hbtrie.c
    src/HBTrie/bs_array.c
    src/HBTrie/bnode.c

    # Time
    src/Time/ticker.c
    src/Time/wheel.c
    src/Time/debouncer.c

    # Workers
    src/Workers/error.c
    src/Workers/priority.c
    src/Workers/transaction_id.c
    src/Workers/work.c
    src/Workers/promise.c
    src/Workers/queue.c
    src/Workers/pool.c

    # Storage
    src/Storage/section.c
    src/Storage/sections.c
    src/Storage/node_serializer.c

    # Database
    src/Database/wal.c
    src/Database/database_lru.c
    src/Database/database.c
)

# Create WaveDB library
add_library(wavedb STATIC ${WAVEDB_SOURCES})
target_link_libraries(wavedb PUBLIC xxhash cbor)

# Hashmap library
add_library(hashmap STATIC
    deps/hashmap/src/hashmap.c
)
target_include_directories(hashmap PUBLIC ${CMAKE_SOURCE_DIR}/deps/hashmap/include)

# Link hashmap to wavedb
target_link_libraries(wavedb PUBLIC hashmap)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(wavedb PRIVATE _WIN32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(wavedb PUBLIC Threads::Threads)
endif()

# GoogleTest
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()

    # Add googletest
    add_subdirectory(deps/googletest)

    # Test for chunk
    add_executable(test_chunk tests/test_chunk.cpp)
    target_link_libraries(test_chunk wavedb gtest gtest_main)
    add_test(NAME test_chunk COMMAND test_chunk)

    # Test for identifier
    add_executable(test_identifier tests/test_identifier.cpp)
    target_link_libraries(test_identifier wavedb gtest gtest_main)
    add_test(NAME test_identifier COMMAND test_identifier)

    # Test for bnode
    add_executable(test_bnode tests/test_bnode.cpp)
    target_link_libraries(test_bnode wavedb gtest gtest_main)
    add_test(NAME test_bnode COMMAND test_bnode)

    # Test for hbtrie
    add_executable(test_hbtrie tests/test_hbtrie.cpp)
    target_link_libraries(test_hbtrie wavedb gtest gtest_main)
    add_test(NAME test_hbtrie COMMAND test_hbtrie)

    # Test for database
    add_executable(test_database tests/test_database.cpp)
    target_link_libraries(test_database wavedb gtest gtest_main)
    add_test(NAME test_database COMMAND test_database)

    # Test for transaction_id
    add_executable(test_transaction_id tests/test_transaction_id.cpp)
    target_link_libraries(test_transaction_id wavedb gtest gtest_main)
    add_test(NAME test_transaction_id COMMAND test_transaction_id)

    # Test for section variable-size storage
    add_executable(test_section_variable tests/test_section_variable.cpp)
    target_link_libraries(test_section_variable wavedb gtest gtest_main)
    add_test(NAME test_section_variable COMMAND test_section_variable)

    # Stress tests
    option(BUILD_STRESS_TESTS "Build stress tests" OFF)
    if(BUILD_STRESS_TESTS)
        # Concurrent sections stress test
        add_executable(test_stress_concurrent tests/stress/test_concurrent_sections.cpp)
        target_link_libraries(test_stress_concurrent wavedb gtest gtest_main Threads::Threads)
        add_test(NAME test_stress_concurrent COMMAND test_stress_concurrent)

        # Resource limits stress test
        add_executable(test_stress_resources tests/stress/test_resource_limits.cpp)
        target_link_libraries(test_stress_resources wavedb gtest gtest_main Threads::Threads)
        add_test(NAME test_stress_resources COMMAND test_stress_resources)

        # Long-running stability test
        add_executable(test_stress_longrun tests/stress/test_long_running.cpp)
        target_link_libraries(test_stress_longrun wavedb gtest gtest_main Threads::Threads)
        add_test(NAME test_stress_longrun COMMAND test_stress_longrun)
    endif()
endif()

# Benchmarks
option(BUILD_BENCHMARKS "Build benchmarks" OFF)
if(BUILD_BENCHMARKS)
    # Benchmark base library
    add_library(benchmark_base STATIC tests/benchmark/benchmark_base.c)
    target_link_libraries(benchmark_base wavedb Threads::Threads)

    # WAL benchmarks
    add_executable(benchmark_wal tests/benchmark/benchmark_wal.cpp)
    target_link_libraries(benchmark_wal benchmark_base wavedb Threads::Threads)

    # Section benchmarks
    add_executable(benchmark_sections tests/benchmark/benchmark_sections.cpp)
    target_link_libraries(benchmark_sections benchmark_base wavedb Threads::Threads)

    # Database benchmarks
    add_executable(benchmark_database tests/benchmark/benchmark_database.cpp)
    target_link_libraries(benchmark_database benchmark_base wavedb Threads::Threads)

    # Create benchmark output directory
    add_custom_target(benchmark_output_dir
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/.benchmarks
        COMMENT "Creating benchmark output directory"
    )

    # Run individual benchmarks
    add_custom_target(run_benchmark_wal
        COMMAND benchmark_wal
        DEPENDS benchmark_wal benchmark_output_dir
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running WAL benchmarks"
    )

    add_custom_target(run_benchmark_sections
        COMMAND benchmark_sections
        DEPENDS benchmark_sections benchmark_output_dir
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running Section benchmarks"
    )

    add_custom_target(run_benchmark_database
        COMMAND benchmark_database
        DEPENDS benchmark_database benchmark_output_dir
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running Database benchmarks (note: may have lock errors when run sequentially)"
    )

    # Run all benchmarks
    add_custom_target(run_benchmarks
        COMMAND echo "=========================================="
        COMMAND echo "Running All Benchmarks"
        COMMAND echo "=========================================="
        COMMAND echo ""
        COMMAND ${CMAKE_COMMAND} -E echo "WAL Benchmarks:"
        COMMAND benchmark_wal
        COMMAND echo ""
        COMMAND ${CMAKE_COMMAND} -E echo "Section Benchmarks:"
        COMMAND benchmark_sections
        COMMAND echo ""
        COMMAND ${CMAKE_COMMAND} -E echo "Database Benchmarks:"
        COMMAND benchmark_database
        DEPENDS benchmark_wal benchmark_sections benchmark_database benchmark_output_dir
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running all benchmarks (WAL, Sections, Database)"
    )

    # Alias for convenience
    add_custom_target(benchmark DEPENDS run_benchmarks)

    # Run benchmarks with JSON output
    add_custom_target(run_benchmarks_json
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/.benchmarks
        COMMAND benchmark_wal > ${CMAKE_SOURCE_DIR}/.benchmarks/wal_results.txt 2>&1
        COMMAND benchmark_sections > ${CMAKE_SOURCE_DIR}/.benchmarks/sections_results.txt 2>&1
        COMMAND benchmark_database > ${CMAKE_SOURCE_DIR}/.benchmarks/database_results.txt 2>&1
        DEPENDS benchmark_wal benchmark_sections benchmark_database
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running benchmarks and saving results to .benchmarks/"
    )
endif()

# Stress tests
option(BUILD_STRESS_TESTS "Build stress tests" OFF)
if(BUILD_STRESS_TESTS)
    # Stress test for concurrent sections
    add_executable(test_concurrent_sections tests/stress/test_concurrent_sections.cpp)
    target_link_libraries(test_concurrent_sections wavedb gtest gtest_main Threads::Threads)

    # Stress test for resource limits
    add_executable(test_resource_limits tests/stress/test_resource_limits.cpp)
    target_link_libraries(test_resource_limits wavedb gtest gtest_main Threads::Threads)

    # Stress test for long-running stability
    add_executable(test_long_running tests/stress/test_long_running.cpp)
    target_link_libraries(test_long_running wavedb gtest gtest_main Threads::Threads)
endif()

# Optional: Build examples
option(BUILD_EXAMPLES "Build examples" OFF)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()