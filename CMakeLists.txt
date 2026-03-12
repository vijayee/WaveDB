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
    src/Workers/work.c
    src/Workers/promise.c
    src/Workers/queue.c
    src/Workers/pool.c

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
endif()

# Optional: Build examples
option(BUILD_EXAMPLES "Build examples" OFF)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()