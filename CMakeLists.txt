cmake_minimum_required(VERSION 3.14)
project(WaveDB VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror=return-type")
set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O3")

# Position Independent Code for shared library linking
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Installation support
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# xxHash library
add_library(xxhash STATIC
    deps/xxhash/xxhash.c
)
target_include_directories(xxhash
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/deps/xxhash>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# libcbor library - use libcbor's CMake build system
# Disable LTO for cbor to avoid LLVM bitcode issues with static linking
# Must set _RELEASE specifically because libcbor only checks that variable
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF CACHE BOOL "Disable LTO in Release" FORCE)
set(CBOR_PRETTY_PRINTER ON CACHE BOOL "Include a pretty-printing routine" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build as a shared library" FORCE)
set(WITH_TESTS OFF CACHE BOOL "Build unit tests" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
add_subdirectory(deps/libcbor)

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
    src/HBTrie/hbtrie_viz.c
    src/HBTrie/mvcc.c

    # Time
    src/Time/ticker.c
    src/Time/wheel.c
    src/Time/debouncer.c

    # Workers
    src/Workers/error.c
    src/Workers/transaction_id.c
    src/Workers/work.c
    src/Workers/promise.c
    src/Workers/queue.c
    src/Workers/pool.c
    src/Workers/join.c

    # Storage
    src/Storage/section.c
    src/Storage/sections.c
    src/Storage/node_serializer.c

    # Database
    src/Database/wal.c
    src/Database/wal_manager.c
    src/Database/wal_compactor.c
    src/Database/database_lru.c
    src/Database/database_config.c
    src/Database/database.c
    src/Database/database_iterator.c
    src/Database/batch.c

    # Layers - GraphQL
    src/Layers/graphql/graphql_types.c
    src/Layers/graphql/graphql_lexer.c
    src/Layers/graphql/graphql_parser.c
    src/Layers/graphql/graphql_schema.c
    src/Layers/graphql/graphql_result.c
    src/Layers/graphql/graphql_plan.c
    src/Layers/graphql/graphql_resolve.c
)

# Create WaveDB library
add_library(wavedb STATIC ${WAVEDB_SOURCES})

# Use atomic operations for reference counting (eliminates lock overhead)
target_compile_definitions(wavedb PUBLIC REFCOUNTER_ATOMIC)

# Link with atomic library for C11 atomics
target_link_libraries(wavedb PUBLIC atomic)

# Modern CMake: Use target-specific include directories
target_include_directories(wavedb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/deps/xxhash
        ${CMAKE_CURRENT_SOURCE_DIR}/deps/hashmap/include
        ${CMAKE_CURRENT_SOURCE_DIR}/deps/libcbor/src
)

target_link_libraries(wavedb PUBLIC xxhash cbor)

# Enable atomic reference counting for better performance and thread safety
target_compile_definitions(wavedb PRIVATE REFCOUNTER_ATOMIC)

# Create namespaced alias for consumers
add_library(WaveDB::wavedb ALIAS wavedb)

# Hashmap library
add_library(hashmap STATIC
    deps/hashmap/src/hashmap.c
)
target_include_directories(hashmap
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/deps/hashmap/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

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

    # Test for LRU memory-based eviction
    add_executable(test_lru_memory tests/test_lru_memory.cpp)
    target_link_libraries(test_lru_memory wavedb gtest gtest_main)
    add_test(NAME test_lru_memory COMMAND test_lru_memory)

    # Test for transaction_id
    add_executable(test_transaction_id tests/test_transaction_id.cpp)
    target_link_libraries(test_transaction_id wavedb gtest gtest_main)
    add_test(NAME test_transaction_id COMMAND test_transaction_id)

    # Test for section variable-size storage
    add_executable(test_section_variable tests/test_section_variable.cpp)
    target_link_libraries(test_section_variable wavedb gtest gtest_main)
    add_test(NAME test_section_variable COMMAND test_section_variable)

    # Test for section GC deallocation and persistence
    add_executable(test_section_gc tests/test_section_gc.cpp)
    target_link_libraries(test_section_gc wavedb gtest gtest_main Threads::Threads)
    add_test(NAME test_section_gc COMMAND test_section_gc)

    # Test for memory pool
    add_executable(test_memory_pool tests/test_memory_pool.cpp)
    target_link_libraries(test_memory_pool wavedb gtest gtest_main)
    add_test(NAME test_memory_pool COMMAND test_memory_pool)

    # Test for MVCC
    add_executable(test_mvcc tests/test_mvcc.cpp)
    target_link_libraries(test_mvcc wavedb gtest gtest_main Threads::Threads)
    add_test(NAME test_mvcc COMMAND test_mvcc)

    # Test for abort cleanup
    add_executable(test_abort_cleanup tests/test_abort_cleanup.cpp)
    target_link_libraries(test_abort_cleanup wavedb gtest gtest_main Threads::Threads)
    add_test(NAME test_abort_cleanup COMMAND test_abort_cleanup)

    # Test for WAL manager
    add_executable(test_wal_manager tests/test_wal_manager.cpp)
    target_link_libraries(test_wal_manager wavedb gtest gtest_main Threads::Threads)
    add_test(NAME test_wal_manager COMMAND test_wal_manager)

    # Test for batch
    add_executable(test_batch tests/test_batch.cpp)
    target_link_libraries(test_batch wavedb gtest gtest_main)
    add_test(NAME test_batch COMMAND test_batch)

    # Test for database_config
    add_executable(test_database_config tests/test_database_config.cpp)
    target_link_libraries(test_database_config wavedb gtest gtest_main)
    add_test(NAME test_database_config COMMAND test_database_config)

    # Test for GraphQL lexer
    add_executable(test_graphql_lexer tests/test_graphql_lexer.cpp)
    target_link_libraries(test_graphql_lexer wavedb gtest gtest_main)
    add_test(NAME test_graphql_lexer COMMAND test_graphql_lexer)

    # Test for GraphQL parser
    add_executable(test_graphql_parser tests/test_graphql_parser.cpp)
    target_link_libraries(test_graphql_parser wavedb gtest gtest_main)
    add_test(NAME test_graphql_parser COMMAND test_graphql_parser)

    # Test for GraphQL schema
    add_executable(test_graphql_schema tests/test_graphql_schema.cpp)
    target_link_libraries(test_graphql_schema wavedb gtest gtest_main)
    add_test(NAME test_graphql_schema COMMAND test_graphql_schema)

    # Test for GraphQL query parser
    add_executable(test_graphql_query tests/test_graphql_query.cpp)
    target_link_libraries(test_graphql_query wavedb gtest gtest_main)
    add_test(NAME test_graphql_query COMMAND test_graphql_query)

    # Test for GraphQL plan compilation
    add_executable(test_graphql_plan tests/test_graphql_plan.cpp)
    target_link_libraries(test_graphql_plan wavedb gtest gtest_main)
    add_test(NAME test_graphql_plan COMMAND test_graphql_plan)

    # Test for GraphQL resolve (queries and mutations)
    add_executable(test_graphql_resolve tests/test_graphql_resolve.cpp)
    target_link_libraries(test_graphql_resolve wavedb gtest gtest_main)
    add_test(NAME test_graphql_resolve COMMAND test_graphql_resolve)

    # Visualization examples removed (used non-MVCC API, will be revisited)

    # Example programs
    option(BUILD_EXAMPLES "Build example programs" OFF)
    if(BUILD_EXAMPLES)
        # HBTrie visualization example
        add_executable(example_hbtrie_viz examples/example_complex_viz.c)
        target_link_libraries(example_hbtrie_viz wavedb Threads::Threads)
    endif()

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

    # Sync Database Benchmark
    add_executable(benchmark_database_sync tests/benchmark/benchmark_database_sync.cpp)
    target_link_libraries(benchmark_database_sync benchmark_base wavedb Threads::Threads)

    # Thread-local WAL benchmark
    add_executable(benchmark_thread_wal tests/benchmark/benchmark_thread_wal.cpp)
    target_link_libraries(benchmark_thread_wal benchmark_base wavedb Threads::Threads)

    # Thread-local WAL simple benchmark (no timing wheel)
    add_executable(benchmark_thread_wal_simple tests/benchmark/benchmark_thread_wal_simple.cpp)
    target_link_libraries(benchmark_thread_wal_simple benchmark_base wavedb Threads::Threads)

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

# Node.js bindings
option(BUILD_NODEJS_BINDINGS "Build Node.js bindings" OFF)
if(BUILD_NODEJS_BINDINGS)
  add_subdirectory(bindings/nodejs)
endif()

# Dart FFI shared library
option(BUILD_DART_BINDINGS "Build Dart FFI shared library" OFF)
if(BUILD_DART_BINDINGS)
  # Build shared library for Dart FFI
  add_library(wavedb_shared SHARED ${WAVEDB_SOURCES})
  target_compile_definitions(wavedb_shared PUBLIC REFCOUNTER_ATOMIC)
  target_link_libraries(wavedb_shared PUBLIC atomic xxhash cbor hashmap)
  if(NOT WIN32)
    target_link_libraries(wavedb_shared PUBLIC Threads::Threads)
  endif()
  target_include_directories(wavedb_shared
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/deps/xxhash
      ${CMAKE_CURRENT_SOURCE_DIR}/deps/hashmap/include
      ${CMAKE_CURRENT_SOURCE_DIR}/deps/libcbor/src
  )
  # Set output name to match Dart FFI expectations
  set_target_properties(wavedb_shared PROPERTIES
    OUTPUT_NAME "wavedb"
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
  )
endif()

# Installation and packaging
# Install libraries and export targets
install(TARGETS wavedb xxhash hashmap
    EXPORT WaveDBTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install headers (preserving directory structure)
install(DIRECTORY src/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# Install dependency headers
install(FILES
    deps/xxhash/xxhash.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES
    deps/hashmap/include/hashmap.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Generate package config file
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WaveDBConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/WaveDBConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WaveDB
)

# Generate version file
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/WaveDBConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Install package config files
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/WaveDBConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/WaveDBConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WaveDB
)

# Export targets for installation
install(EXPORT WaveDBTargets
    FILE WaveDBTargets.cmake
    NAMESPACE WaveDB::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WaveDB
)
# Test for threading fix
# add_executable(test_threading_fix tests/test_threading_fix.cpp)
# target_link_libraries(test_threading_fix wavedb Threads::Threads gtest gtest_main)
