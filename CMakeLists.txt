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
include_directories(${CMAKE_SOURCE_DIR}/deps/xxhash)
include_directories(${CMAKE_SOURCE_DIR}/deps/hashmap/include)

# xxHash library
add_library(xxhash STATIC
    deps/xxhash/xxhash.c
)
target_include_directories(xxhash PUBLIC ${CMAKE_SOURCE_DIR}/deps/xxhash)

# WaveDB library sources
set(WAVEDB_SOURCES
    # Util
    src/Util/allocator.c
    src/Util/log.c
    src/Util/vec.c
    src/Util/hash.c
    src/Util/threadding.c

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
)

# Create WaveDB library
add_library(wavedb STATIC ${WAVEDB_SOURCES})
target_link_libraries(wavedb PUBLIC xxhash)

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
endif()

# Optional: Build examples
option(BUILD_EXAMPLES "Build examples" OFF)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()