# Node.js bindings CMakeLists.txt
# This provides optional CMake-based build for Node.js bindings
# Note: The primary build method is via node-gyp (npm run build)

cmake_minimum_required(VERSION 3.14)

# Node.js addon sources
set(NODEJS_BINDING_SOURCES
    src/binding.cpp
    src/database.cc
    src/path.cc
    src/identifier.cc
    src/async_worker.cc
    src/put_worker.cc
    src/get_worker.cc
    src/del_worker.cc
    src/batch_worker.cc
    src/iterator.cc
)

# Find Node.js development headers
find_program(NODE_EXECUTABLE node
    NAMES node nodejs
    DOC "Node.js executable"
)

if(NODE_EXECUTABLE)
    execute_process(
        COMMAND ${NODE_EXECUTABLE} -e "console.log(process.versions.modules)"
        OUTPUT_VARIABLE NODE_MODULE_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    execute_process(
        COMMAND ${NODE_EXECUTABLE} -e "console.log(process.arch)"
        OUTPUT_VARIABLE NODE_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    message(STATUS "Node.js module version: ${NODE_MODULE_VERSION}")
    message(STATUS "Node.js architecture: ${NODE_ARCH}")
endif()

# Note: Building Node.js addons requires node-addon-api and proper Node.js headers
# This CMakeLists.txt is provided for reference/integration purposes
# The recommended build method is: cd bindings/nodejs && npm run build

message(STATUS "Node.js bindings configured")
message(STATUS "To build: npm run build")